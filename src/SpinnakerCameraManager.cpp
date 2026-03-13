#include "SpinnakerCameraManager.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

/// Combined GigE link cap: 1 Gbit/s expressed in bytes/s
static constexpr int64_t  GIGE_MAX_BANDWIDTH_BPS = 125'000'000LL;

/// IEEE 802.3 jumbo frame MTU that avoids per-packet overhead
static constexpr uint32_t JUMBO_PACKET_SIZE = 9000;

/// How long GetNextImage() will block before timing out (ms)
static constexpr uint32_t GRAB_TIMEOUT_MS = 1000;

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

SpinnakerCameraManager::SpinnakerCameraManager(SharedMemoryManager& shm)
    : shm_(shm) {}

SpinnakerCameraManager::~SpinnakerCameraManager() {
    Shutdown();
}

bool SpinnakerCameraManager::Initialize(const std::string& save_directory) {
    save_directory_ = save_directory;

    try {
        system_   = Spinnaker::System::GetInstance();
        cam_list_ = system_->GetCameras();

        const unsigned int count = cam_list_.GetSize();
        std::cout << "[CameraManager] Found " << count << " camera(s).\n";

        for (unsigned int i = 0; i < count && i < static_cast<unsigned int>(MAX_CAMERAS); ++i) {
            cameras_.push_back(cam_list_.GetByIndex(i));
            // Init early so the NodeMap is accessible (for GetCameraInfo, SetParameter)
            // even when not acquiring.
            cameras_.back()->Init();
        }

    } catch (const Spinnaker::Exception& ex) {
        std::cerr << "[CameraManager] Initialisation error: " << ex.what() << '\n';
        return false;
    }

    shm_.SetNumCameras(static_cast<int32_t>(cameras_.size()));

    // Start the background disk-save thread regardless of camera count.
    save_thread_running_ = true;
    save_thread_         = std::thread(&SpinnakerCameraManager::DiskSaveLoop, this);

    return true;
}

void SpinnakerCameraManager::Shutdown() {
    StopAcquisition(-1);

    // Drain and terminate the disk-save thread.
    {
        std::lock_guard<std::mutex> lk(save_mutex_);
        save_thread_running_ = false;
    }
    save_cv_.notify_all();
    if (save_thread_.joinable()) save_thread_.join();

    try {
        for (auto& cam : cameras_) {
            if (cam->IsInitialized()) cam->DeInit();
        }
        cameras_.clear();
        cam_list_.Clear();
        if (system_) system_->ReleaseInstance();
    } catch (const Spinnaker::Exception& ex) {
        std::cerr << "[CameraManager] Shutdown error: " << ex.what() << '\n';
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Acquisition control
// ─────────────────────────────────────────────────────────────────────────────

bool SpinnakerCameraManager::StartAcquisition(int32_t camera_id) {
    if (cameras_.empty()) {
        std::cerr << "[CameraManager] Cannot start: no cameras found.\n";
        return false;
    }

    if (camera_id == -1) {
        bool any_ok = false;
        for (int32_t i = 0; i < static_cast<int32_t>(cameras_.size()); ++i)
            any_ok |= StartCamera(i);
        return any_ok;
    }

    if (camera_id < 0 || camera_id >= static_cast<int32_t>(cameras_.size())) {
        std::cerr << "[CameraManager] StartAcquisition: invalid camera_id " << camera_id << '\n';
        return false;
    }
    return StartCamera(camera_id);
}

bool SpinnakerCameraManager::StopAcquisition(int32_t camera_id) {
    if (camera_id == -1) {
        bool all_ok = true;
        for (int32_t i = 0; i < static_cast<int32_t>(cameras_.size()); ++i)
            all_ok &= StopCamera(i);
        return all_ok;
    }

    if (camera_id < 0 || camera_id >= static_cast<int32_t>(cameras_.size())) {
        std::cerr << "[CameraManager] StopAcquisition: invalid camera_id " << camera_id << '\n';
        return false;
    }
    return StopCamera(camera_id);
}

bool SpinnakerCameraManager::StartCamera(int32_t camera_id) {
    if (camera_acquiring_[camera_id].load(std::memory_order_acquire)) return true;

    camera_acquiring_[camera_id].store(true, std::memory_order_release);

    try {
        ConfigureCamera(cameras_[camera_id]);
        cameras_[camera_id]->BeginAcquisition();

        acq_threads_[camera_id] = std::thread(
            &SpinnakerCameraManager::CameraAcquisitionThread,
            this, cameras_[camera_id], camera_id);

    } catch (const Spinnaker::Exception& ex) {
        std::cerr << "[CameraManager] StartCamera(" << camera_id
                  << ") error: " << ex.what() << '\n';
        camera_acquiring_[camera_id].store(false, std::memory_order_release);
        return false;
    }

    std::cout << "[CameraManager] Acquisition started on camera " << camera_id << ".\n";
    return true;
}

bool SpinnakerCameraManager::StopCamera(int32_t camera_id) {
    if (!camera_acquiring_[camera_id].exchange(false, std::memory_order_acq_rel))
        return true;  // was not running

    if (acq_threads_[camera_id].joinable())
        acq_threads_[camera_id].join();

    try {
        if (cameras_[camera_id]->IsStreaming())
            cameras_[camera_id]->EndAcquisition();
    } catch (const Spinnaker::Exception& ex) {
        std::cerr << "[CameraManager] StopCamera(" << camera_id
                  << ") error: " << ex.what() << '\n';
    }

    std::cout << "[CameraManager] Acquisition stopped on camera " << camera_id << ".\n";
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Camera configuration
// ─────────────────────────────────────────────────────────────────────────────

void SpinnakerCameraManager::ConfigureCamera(Spinnaker::CameraPtr& camera) {
    using namespace Spinnaker::GenApi;

    INodeMap& nm = camera->GetNodeMap();

    // ── Continuous acquisition mode ───────────────────────────────────────────
    {
        CEnumerationPtr acqMode = nm.GetNode("AcquisitionMode");
        if (IsAvailable(acqMode) && IsWritable(acqMode)) {
            CEnumEntryPtr entry = acqMode->GetEntryByName("Continuous");
            if (IsAvailable(entry) && IsReadable(entry))
                acqMode->SetIntValue(entry->GetValue());
        }
    }

    // ── Jumbo frames (reduces per-packet overhead at high frame rates) ─────────
    {
        CIntegerPtr pktSize = nm.GetNode("GevSCPSPacketSize");
        if (IsAvailable(pktSize) && IsWritable(pktSize)) {
            const int64_t val = std::min(static_cast<int64_t>(JUMBO_PACKET_SIZE),
                                         pktSize->GetMax());
            pktSize->SetValue(val);
        }
    }

    // ── Per-camera bandwidth throttle ─────────────────────────────────────────
    // Divide the 1 Gbit/s budget equally among all cameras so their combined
    // traffic stays within the NIC's capability.
    {
        const int64_t cam_count = static_cast<int64_t>(
            cameras_.empty() ? 1 : cameras_.size());
        const int64_t per_camera_bps = GIGE_MAX_BANDWIDTH_BPS / cam_count;

        CIntegerPtr limit = nm.GetNode("DeviceLinkThroughputLimit");
        if (IsAvailable(limit) && IsWritable(limit)) {
            const int64_t clamped =
                std::max(limit->GetMin(),
                         std::min(per_camera_bps, limit->GetMax()));
            limit->SetValue(clamped);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-camera acquisition thread
// ─────────────────────────────────────────────────────────────────────────────

void SpinnakerCameraManager::CameraAcquisitionThread(Spinnaker::CameraPtr camera,
                                                     int32_t             camera_id) {
    while (camera_acquiring_[camera_id].load(std::memory_order_acquire)) {

        // ── Grab next image (blocks for up to GRAB_TIMEOUT_MS) ───────────────
        Spinnaker::ImagePtr image;
        try {
            image = camera->GetNextImage(GRAB_TIMEOUT_MS);
        } catch (const Spinnaker::Exception& ex) {
            std::cerr << "[Acquisition " << camera_id << "] GetNextImage: "
                      << ex.what() << '\n';
            continue;
        }

        if (image->IsIncomplete()) {
            std::cerr << "[Acquisition " << camera_id << "] Incomplete image (status "
                      << image->GetImageStatus() << "); dropping.\n";
            image->Release();
            continue;
        }

        // ── Claim a shared memory buffer ──────────────────────────────────────
        const int32_t buf_idx = shm_.ClaimFreeBuffer();
        if (buf_idx < 0) {
            image->Release();
            continue;
        }

        // ── Deep-copy image data into shared memory ───────────────────────────
        const std::size_t src_bytes = image->GetImageSize();
        const std::size_t dst_bytes = shm_.GetHeader()->single_image_size;
        std::memcpy(shm_.GetBufferPtr(buf_idx),
                    image->GetData(),
                    std::min(src_bytes, dst_bytes));

        // ── Publish to consumers ──────────────────────────────────────────────
        shm_.PublishBuffer(buf_idx, camera_id,
                           static_cast<int32_t>(image->GetWidth()),
                           static_cast<int32_t>(image->GetHeight()));
        RecordFrameTime(camera_id);

        // ── Optional disk save (non-blocking) ─────────────────────────────────
        if (pending_save_.exchange(false, std::memory_order_acq_rel)) {
            FrameSnapshot snap;
            snap.width     = static_cast<uint32_t>(image->GetWidth());
            snap.height    = static_cast<uint32_t>(image->GetHeight());
            snap.channels  = 1;  // Mono8; extend here for colour pixel formats
            snap.camera_id = camera_id;
            snap.data.resize(src_bytes);
            std::memcpy(snap.data.data(), image->GetData(), src_bytes);

            // Build filename: frame_cam<N>_<timestamp_ms>.raw
            const auto now = std::chrono::system_clock::now();
            const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now.time_since_epoch()).count();

            std::string save_dir;
            {
                std::lock_guard<std::mutex> lk(save_dir_mutex_);
                save_dir = save_directory_;
            }

            std::ostringstream ss;
            ss << save_dir << "/frame_cam" << camera_id << "_" << ms << ".raw";
            snap.save_path = ss.str();

            {
                std::lock_guard<std::mutex> lk(save_mutex_);
                save_queue_.push(std::move(snap));
            }
            save_cv_.notify_one();
        }

        // ── Release the Spinnaker ImagePtr immediately ─────────────────────────
        image->Release();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Disk-save background thread
// ─────────────────────────────────────────────────────────────────────────────

void SpinnakerCameraManager::DiskSaveLoop() {
    while (true) {
        FrameSnapshot snap;

        {
            std::unique_lock<std::mutex> lk(save_mutex_);
            save_cv_.wait(lk, [this] {
                return !save_queue_.empty() || !save_thread_running_.load();
            });

            if (!save_thread_running_ && save_queue_.empty()) break;
            if (save_queue_.empty()) continue;

            snap = std::move(save_queue_.front());
            save_queue_.pop();
        }

        // Raw binary dump.  Extend here to write PNG/TIFF using an image library.
        if (FILE* fp = std::fopen(snap.save_path.c_str(), "wb")) {
            std::fwrite(snap.data.data(), 1, snap.data.size(), fp);
            std::fclose(fp);
            std::cout << "[DiskSave] cam" << snap.camera_id
                      << " wrote " << snap.data.size()
                      << " bytes to " << snap.save_path << '\n';
        } else {
            std::cerr << "[DiskSave] Failed to open: " << snap.save_path << '\n';
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// FPS tracking (per-camera)
// ─────────────────────────────────────────────────────────────────────────────

void SpinnakerCameraManager::RecordFrameTime(int32_t camera_id) {
    const int64_t now_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
        .count();

    std::lock_guard<std::mutex> lk(camera_fps_mutex_[camera_id]);
    auto& times = camera_frame_times_[camera_id];
    times.push_back(now_us);
    while (times.size() > FPS_WINDOW)
        times.pop_front();

    if (times.size() >= 2) {
        const double elapsed_us =
            static_cast<double>(times.back() - times.front());
        const double fps =
            (static_cast<double>(times.size() - 1) / elapsed_us) * 1e6;
        camera_fps_[camera_id].store(static_cast<float>(fps),
                                     std::memory_order_relaxed);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Parameter control
// ─────────────────────────────────────────────────────────────────────────────

bool SpinnakerCameraManager::SetParameter(const std::string& param_name,
                                          float              float_value,
                                          int32_t            int_value,
                                          int32_t            camera_id) {
    using namespace Spinnaker::GenApi;

    bool any_success = false;

    for (int32_t i = 0; i < static_cast<int32_t>(cameras_.size()); ++i) {
        if (camera_id != -1 && i != camera_id) continue;

        auto& camera = cameras_[i];
        if (!camera->IsInitialized()) continue;

        INodeMap& nm = camera->GetNodeMap();

        // Try as float (CFloatPtr covers ExposureTime, Gain, Gamma, …)
        {
            CFloatPtr node = nm.GetNode(param_name.c_str());
            if (IsAvailable(node) && IsWritable(node)) {
                try {
                    const double clamped =
                        std::max(node->GetMin(),
                                 std::min(static_cast<double>(float_value),
                                          node->GetMax()));
                    node->SetValue(clamped);
                    any_success = true;
                    continue;
                } catch (const Spinnaker::Exception& ex) {
                    std::cerr << "[SetParameter] Float set failed for '"
                              << param_name << "' on cam " << i
                              << ": " << ex.what() << '\n';
                }
            }
        }

        // Try as integer (CIntegerPtr covers Width, Height, OffsetX, …)
        {
            CIntegerPtr node = nm.GetNode(param_name.c_str());
            if (IsAvailable(node) && IsWritable(node)) {
                try {
                    const int64_t clamped =
                        std::max(node->GetMin(),
                                 std::min(static_cast<int64_t>(int_value),
                                          node->GetMax()));
                    node->SetValue(clamped);
                    any_success = true;
                } catch (const Spinnaker::Exception& ex) {
                    std::cerr << "[SetParameter] Int set failed for '"
                              << param_name << "' on cam " << i
                              << ": " << ex.what() << '\n';
                }
            }
        }
    }

    return any_success;
}

// ─────────────────────────────────────────────────────────────────────────────
// Camera info query
// ─────────────────────────────────────────────────────────────────────────────

bool SpinnakerCameraManager::GetCameraInfo(int32_t camera_id, CameraInfo& info) {
    if (camera_id < 0 || camera_id >= static_cast<int32_t>(cameras_.size()))
        return false;

    auto& cam = cameras_[camera_id];
    if (!cam->IsInitialized()) return false;

    using namespace Spinnaker::GenApi;
    INodeMap& nm = cam->GetNodeMap();

    info.camera_id = camera_id;
    info.acquiring = IsCameraAcquiring(camera_id);
    info.fps       = camera_fps_[camera_id].load(std::memory_order_relaxed);

    // Helper lambdas for safe node reads
    auto readStr = [&](const char* node_name) -> std::string {
        CStringPtr p = nm.GetNode(node_name);
        if (IsAvailable(p) && IsReadable(p))
            return std::string(p->GetValue().c_str());
        return {};
    };

    auto readInt = [&](const char* node_name, int32_t default_val = 0) -> int32_t {
        CIntegerPtr p = nm.GetNode(node_name);
        if (IsAvailable(p) && IsReadable(p))
            return static_cast<int32_t>(p->GetValue());
        return default_val;
    };

    auto readFloat = [&](const char* node_name) -> float {
        CFloatPtr p = nm.GetNode(node_name);
        if (IsAvailable(p) && IsReadable(p))
            return static_cast<float>(p->GetValue());
        return 0.0f;
    };

    info.model_name = readStr("DeviceModelName");
    info.serial     = readStr("DeviceSerialNumber");

    // GevCurrentIPAddress: 32-bit big-endian integer → dotted-decimal
    {
        CIntegerPtr p = nm.GetNode("GevCurrentIPAddress");
        if (IsAvailable(p) && IsReadable(p)) {
            const auto ip = static_cast<uint32_t>(p->GetValue());
            char buf[20];
            std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                          (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                          (ip >>  8) & 0xFF,  ip        & 0xFF);
            info.ip_address = buf;
        }
    }

    info.width    = readInt("Width");
    info.height   = readInt("Height");
    info.offset_x = readInt("OffsetX");
    info.offset_y = readInt("OffsetY");

    // BinningHorizontal/Vertical; fall back to BinningX/Y if needed
    {
        auto readBinning = [&](const char* a, const char* b) -> int32_t {
            int32_t v = readInt(a, 0);
            if (v == 0) v = readInt(b, 0);
            return (v <= 0) ? 1 : v;
        };
        info.binning_h = readBinning("BinningHorizontal", "BinningX");
        info.binning_v = readBinning("BinningVertical",   "BinningY");
    }

    info.exposure_us = readFloat("ExposureTime");
    info.gain_db     = readFloat("Gain");

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Save directory
// ─────────────────────────────────────────────────────────────────────────────

void SpinnakerCameraManager::SetSaveDirectory(const std::string& path) {
    std::lock_guard<std::mutex> lk(save_dir_mutex_);
    save_directory_ = path;
    std::cout << "[CameraManager] Save directory set to: " << path << '\n';
}

// ─────────────────────────────────────────────────────────────────────────────
// Misc queries
// ─────────────────────────────────────────────────────────────────────────────

void SpinnakerCameraManager::TriggerDiskSave() {
    pending_save_.store(true, std::memory_order_release);
}

int32_t SpinnakerCameraManager::GetConnectedCameraCount() const {
    return static_cast<int32_t>(cameras_.size());
}

float SpinnakerCameraManager::GetCurrentFPS() const {
    float total = 0.0f;
    for (int32_t i = 0; i < static_cast<int32_t>(cameras_.size()); ++i)
        total += camera_fps_[i].load(std::memory_order_relaxed);
    return total;
}

bool SpinnakerCameraManager::IsAcquiring() const {
    for (int32_t i = 0; i < static_cast<int32_t>(cameras_.size()); ++i)
        if (camera_acquiring_[i].load(std::memory_order_acquire)) return true;
    return false;
}

bool SpinnakerCameraManager::IsCameraAcquiring(int32_t camera_id) const {
    if (camera_id < 0 || camera_id >= static_cast<int32_t>(cameras_.size()))
        return false;
    return camera_acquiring_[camera_id].load(std::memory_order_acquire);
}
