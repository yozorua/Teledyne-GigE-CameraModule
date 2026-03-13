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

        for (unsigned int i = 0; i < count; ++i)
            cameras_.push_back(cam_list_.GetByIndex(i));

    } catch (const Spinnaker::Exception& ex) {
        std::cerr << "[CameraManager] Initialisation error: " << ex.what() << '\n';
        return false;
    }

    // Start the background disk-save thread regardless of camera count.
    save_thread_running_ = true;
    save_thread_         = std::thread(&SpinnakerCameraManager::DiskSaveLoop, this);

    return true;
}

void SpinnakerCameraManager::Shutdown() {
    StopAcquisition();

    // Drain and terminate the disk-save thread
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

bool SpinnakerCameraManager::StartAcquisition() {
    if (acquiring_.load()) return true;

    if (cameras_.empty()) {
        std::cerr << "[CameraManager] Cannot start: no cameras found.\n";
        return false;
    }

    acquiring_ = true;

    shm_.SetNumCameras(static_cast<int32_t>(cameras_.size()));

    try {
        for (std::size_t i = 0; i < cameras_.size(); ++i) {
            cameras_[i]->Init();
            ConfigureCamera(cameras_[i]);
            cameras_[i]->BeginAcquisition();

            // Each camera gets its own dedicated acquisition thread.
            // Pass camera_id (= position in the cameras_ vector) explicitly so
            // PublishBuffer can update the per-camera latest index.
            acq_threads_.emplace_back(
                &SpinnakerCameraManager::CameraAcquisitionThread,
                this, cameras_[i], static_cast<int32_t>(i));
        }
    } catch (const Spinnaker::Exception& ex) {
        std::cerr << "[CameraManager] StartAcquisition error: " << ex.what() << '\n';
        acquiring_ = false;
        return false;
    }

    std::cout << "[CameraManager] Acquisition started on "
              << cameras_.size() << " camera(s).\n";
    return true;
}

bool SpinnakerCameraManager::StopAcquisition() {
    if (!acquiring_.exchange(false)) return true;

    for (auto& t : acq_threads_) {
        if (t.joinable()) t.join();
    }
    acq_threads_.clear();

    try {
        for (auto& camera : cameras_) {
            if (camera->IsStreaming())    camera->EndAcquisition();
            if (camera->IsInitialized()) camera->DeInit();
        }
    } catch (const Spinnaker::Exception& ex) {
        std::cerr << "[CameraManager] StopAcquisition error: " << ex.what() << '\n';
    }

    std::cout << "[CameraManager] Acquisition stopped.\n";
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
    while (acquiring_.load(std::memory_order_acquire)) {

        // ── Grab next image (blocks for up to GRAB_TIMEOUT_MS) ───────────────
        Spinnaker::ImagePtr image;
        try {
            image = camera->GetNextImage(GRAB_TIMEOUT_MS);
        } catch (const Spinnaker::Exception& ex) {
            std::cerr << "[Acquisition] GetNextImage: " << ex.what() << '\n';
            continue;
        }

        if (image->IsIncomplete()) {
            std::cerr << "[Acquisition] Incomplete image (status "
                      << image->GetImageStatus() << "); dropping.\n";
            image->Release();
            continue;
        }

        // ── Claim a shared memory buffer ──────────────────────────────────────
        const int32_t buf_idx = shm_.ClaimFreeBuffer();
        if (buf_idx < 0) {
            // All buffers are pinned by slow consumers; drop this frame rather
            // than blocking the camera SDK callback/polling loop.
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
        shm_.PublishBuffer(buf_idx, camera_id);
        RecordFrameTime();

        // ── Optional disk save (non-blocking) ─────────────────────────────────
        if (pending_save_.exchange(false, std::memory_order_acq_rel)) {
            FrameSnapshot snap;
            snap.width    = static_cast<uint32_t>(image->GetWidth());
            snap.height   = static_cast<uint32_t>(image->GetHeight());
            snap.channels = 1; // Mono8; extend here for colour pixel formats
            snap.data.resize(src_bytes);
            std::memcpy(snap.data.data(), image->GetData(), src_bytes);

            // Build filename from current wall-clock time
            const auto now = std::chrono::system_clock::now();
            const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now.time_since_epoch()).count();
            std::ostringstream ss;
            ss << save_directory_ << "/frame_" << ms << ".raw";
            snap.save_path = ss.str();

            {
                std::lock_guard<std::mutex> lk(save_mutex_);
                save_queue_.push(std::move(snap));
            }
            save_cv_.notify_one();
        }

        // ── Release the Spinnaker ImagePtr immediately ─────────────────────────
        // This returns the internal buffer to the SDK so it can receive the next
        // frame without any head-of-line blocking.
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
            std::cout << "[DiskSave] Wrote " << snap.data.size()
                      << " bytes to " << snap.save_path << '\n';
        } else {
            std::cerr << "[DiskSave] Failed to open: " << snap.save_path << '\n';
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// FPS tracking
// ─────────────────────────────────────────────────────────────────────────────

void SpinnakerCameraManager::RecordFrameTime() {
    const int64_t now_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
        .count();

    std::lock_guard<std::mutex> lk(fps_mutex_);
    frame_times_us_.push_back(now_us);
    while (frame_times_us_.size() > FPS_WINDOW)
        frame_times_us_.pop_front();

    if (frame_times_us_.size() >= 2) {
        const double elapsed_us =
            static_cast<double>(frame_times_us_.back() - frame_times_us_.front());
        const double fps =
            (static_cast<double>(frame_times_us_.size() - 1) / elapsed_us) * 1e6;
        current_fps_.store(static_cast<float>(fps), std::memory_order_relaxed);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Parameter control
// ─────────────────────────────────────────────────────────────────────────────

bool SpinnakerCameraManager::SetParameter(const std::string& param_name,
                                          float              float_value,
                                          int32_t            int_value) {
    using namespace Spinnaker::GenApi;

    bool any_success = false;

    for (auto& camera : cameras_) {
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
                    continue; // move on to next camera
                } catch (const Spinnaker::Exception& ex) {
                    std::cerr << "[SetParameter] Float set failed for '"
                              << param_name << "': " << ex.what() << '\n';
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
                              << param_name << "': " << ex.what() << '\n';
                }
            }
        }
    }

    return any_success;
}

// ─────────────────────────────────────────────────────────────────────────────
// Misc
// ─────────────────────────────────────────────────────────────────────────────

void SpinnakerCameraManager::TriggerDiskSave() {
    pending_save_.store(true, std::memory_order_release);
}

int32_t SpinnakerCameraManager::GetConnectedCameraCount() const {
    return static_cast<int32_t>(cameras_.size());
}

float SpinnakerCameraManager::GetCurrentFPS() const {
    return current_fps_.load(std::memory_order_relaxed);
}
