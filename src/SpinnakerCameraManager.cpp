#include "SpinnakerCameraManager.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

// Windows Imaging Component — used by DiskSaveLoop to write JPEG files.
// These are Windows system libraries; no additional install is required.
#include <wincodec.h>
#include <wrl/client.h>   // Microsoft::WRL::ComPtr

// Spinnaker image processor (debayer / pixel-format conversion)
#include "ImageProcessor.h"

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

/// Combined 10 GigE link cap: 10 Gbit/s expressed in bytes/s
static constexpr int64_t  GIGE_MAX_BANDWIDTH_BPS = 1'250'000'000LL;

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

        debayer_running_[camera_id].store(true, std::memory_order_release);
        debayer_threads_[camera_id] = std::thread(
            &SpinnakerCameraManager::DebayerThread, this, camera_id);

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

    // 1. Stop the acquisition thread first (no more raw frames will be posted).
    if (acq_threads_[camera_id].joinable())
        acq_threads_[camera_id].join();

    // 2. Signal the debayer thread and wait for it to drain.
    {
        std::lock_guard<std::mutex> lk(raw_mutex_[camera_id]);
        debayer_running_[camera_id].store(false, std::memory_order_release);
    }
    raw_cv_[camera_id].notify_one();
    if (debayer_threads_[camera_id].joinable())
        debayer_threads_[camera_id].join();

    // 3. End acquisition on the camera.
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

        // ── Copy raw Bayer bytes into the debayer slot ─────────────────────────
        // This is the only work done while holding the ImagePtr.
        // The debayer thread picks up the raw bytes and does conversion
        // asynchronously, so image->Release() happens before any CPU-heavy work.
        {
            const std::size_t raw_bytes = image->GetImageSize();
            std::lock_guard<std::mutex> lk(raw_mutex_[camera_id]);
            RawFrame& slot = raw_frames_[camera_id];
            if (slot.data.size() < raw_bytes)
                slot.data.resize(raw_bytes);
            std::memcpy(slot.data.data(), image->GetData(), raw_bytes);
            slot.width        = static_cast<int32_t>(image->GetWidth());
            slot.height       = static_cast<int32_t>(image->GetHeight());
            slot.pixel_format = image->GetPixelFormat();
            slot.pending      = true;
        }

        // ── Release the Spinnaker ImagePtr — before any debayer work ──────────
        image->Release();

        raw_cv_[camera_id].notify_one();
        RecordFrameTime(camera_id);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-camera debayer thread
// Converts raw Bayer frames to RGB8, writes to SHM, handles disk saves.
// Runs independently of the acquisition thread so debayer latency does not
// delay image->Release() or affect the camera's internal buffer queue.
// ─────────────────────────────────────────────────────────────────────────────

void SpinnakerCameraManager::DebayerThread(int32_t camera_id) {
    Spinnaker::ImageProcessor img_proc;
    img_proc.SetColorProcessing(
        Spinnaker::SPINNAKER_COLOR_PROCESSING_ALGORITHM_HQ_LINEAR);

    while (true) {
        RawFrame local;

        {
            std::unique_lock<std::mutex> lk(raw_mutex_[camera_id]);
            raw_cv_[camera_id].wait(lk, [&] {
                return raw_frames_[camera_id].pending ||
                       !debayer_running_[camera_id].load(std::memory_order_relaxed);
            });

            if (!raw_frames_[camera_id].pending) break;  // shutdown with no pending frame

            // Move the slot contents out so we release the lock before debayering.
            local = std::move(raw_frames_[camera_id]);
            raw_frames_[camera_id].pending = false;
        }

        // ── Debayer: wrap raw bytes in a Spinnaker Image, convert to RGB8 ─────
        // Image::Create() references the provided buffer without copying.
        // ImageProcessor::Convert() returns a new independent heap ImagePtr.
        // Neither call requires an active camera stream.
        Spinnaker::ImagePtr raw_img = Spinnaker::Image::Create(
            static_cast<size_t>(local.width),
            static_cast<size_t>(local.height),
            0, 0,
            local.pixel_format,
            local.data.data());

        Spinnaker::ImagePtr rgb_img = img_proc.Convert(raw_img, Spinnaker::PixelFormat_RGB8);

        // ── Write RGB8 frame to shared memory ─────────────────────────────────
        const int32_t buf_idx = shm_.ClaimFreeBuffer();
        if (buf_idx >= 0) {
            const std::size_t rgb_bytes = rgb_img->GetImageSize();
            const std::size_t dst_bytes = shm_.GetHeader()->single_image_size;
            std::memcpy(shm_.GetBufferPtr(buf_idx),
                        rgb_img->GetData(),
                        std::min(rgb_bytes, dst_bytes));
            shm_.PublishBuffer(buf_idx, camera_id, local.width, local.height);
        }

        // ── Optional disk save ─────────────────────────────────────────────────
        // The RGB8 data is already available here — no second conversion needed.
        int32_t save_req = pending_save_camera_id_.load(std::memory_order_acquire);
        const bool save_wanted = (save_req != SAVE_IDLE) &&
                                 (save_req == camera_id || save_req == -1);
        if (save_wanted &&
            pending_save_camera_id_.compare_exchange_strong(
                save_req, SAVE_IDLE,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {

            FrameSnapshot snap;
            snap.camera_id = camera_id;
            snap.width     = static_cast<uint32_t>(rgb_img->GetWidth());
            snap.height    = static_cast<uint32_t>(rgb_img->GetHeight());
            snap.channels  = 3;
            const std::size_t rgb_bytes = rgb_img->GetImageSize();
            snap.data.resize(rgb_bytes);
            std::memcpy(snap.data.data(), rgb_img->GetData(), rgb_bytes);

            const auto now = std::chrono::system_clock::now();
            const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now.time_since_epoch()).count();
            std::string save_dir;
            {
                std::lock_guard<std::mutex> lk(save_dir_mutex_);
                save_dir = save_directory_;
            }
            std::ostringstream ss;
            ss << save_dir << "/frame_cam" << camera_id << "_" << ms << ".jpg";
            snap.save_path = ss.str();

            {
                std::lock_guard<std::mutex> lk(save_mutex_);
                save_queue_.push(std::move(snap));
            }
            save_cv_.notify_one();
        }
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

        // Write JPEG via Windows Imaging Component (WIC).
        // The pixel data in snap.data is already debayered RGB8 (3 channels,
        // R first) from the conversion done in the acquisition thread.
        // WIC GUID_WICPixelFormat24bppRGB expects the same R-G-B byte order.
        [&] {
            using Microsoft::WRL::ComPtr;

            // COM must be initialised on each thread that uses WIC.
            const HRESULT hr_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            const bool com_owned  = SUCCEEDED(hr_init);

            auto cleanup_com = [&] { if (com_owned) CoUninitialize(); };

            ComPtr<IWICImagingFactory> factory;
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&factory)))) {
                std::cerr << "[DiskSave] WIC factory creation failed.\n";
                cleanup_com();
                return;
            }

            ComPtr<IWICStream> wic_stream;
            factory->CreateStream(&wic_stream);

            // IWICStream::InitializeFromFilename requires a wide-char path.
            const std::wstring wpath(snap.save_path.begin(), snap.save_path.end());
            if (FAILED(wic_stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE))) {
                std::cerr << "[DiskSave] Cannot create file: " << snap.save_path << '\n';
                cleanup_com();
                return;
            }

            ComPtr<IWICBitmapEncoder> encoder;
            factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
            encoder->Initialize(wic_stream.Get(), WICBitmapEncoderNoCache);

            ComPtr<IWICBitmapFrameEncode> frame;
            ComPtr<IPropertyBag2>         props;
            encoder->CreateNewFrame(&frame, &props);

            // Set JPEG quality to 95 via IPropertyBag2.
            PROPBAG2 opt{};
            opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT val{};
            val.vt     = VT_R4;
            val.fltVal = 0.95f;
            props->Write(1, &opt, &val);

            frame->Initialize(props.Get());
            frame->SetSize(snap.width, snap.height);

            WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppRGB;
            frame->SetPixelFormat(&fmt);

            const UINT stride = snap.width * snap.channels;
            frame->WritePixels(snap.height, stride,
                               static_cast<UINT>(snap.data.size()),
                               snap.data.data());
            frame->Commit();
            encoder->Commit();

            std::cout << "[DiskSave] cam" << snap.camera_id
                      << "  " << snap.width << "x" << snap.height
                      << "  RGB  ->  " << snap.save_path << '\n';

            cleanup_com();
        }();
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
                                          int32_t            camera_id,
                                          const std::string& string_value) {
    using namespace Spinnaker::GenApi;

    bool any_success = false;

    for (int32_t i = 0; i < static_cast<int32_t>(cameras_.size()); ++i) {
        if (camera_id != -1 && i != camera_id) continue;

        auto& camera = cameras_[i];
        if (!camera->IsInitialized()) continue;

        INodeMap& nm = camera->GetNodeMap();

        // ── Enumeration node (ExposureAuto, GainAuto, BalanceWhiteAuto, …) ──────
        // Tried first when the caller supplies a non-empty string_value.
        if (!string_value.empty()) {
            CEnumerationPtr node = nm.GetNode(param_name.c_str());
            if (IsAvailable(node) && IsWritable(node)) {
                try {
                    CEnumEntryPtr entry = node->GetEntryByName(string_value.c_str());
                    if (IsAvailable(entry) && IsReadable(entry)) {
                        node->SetIntValue(entry->GetValue());
                        any_success = true;
                        continue;
                    } else {
                        std::cerr << "[SetParameter] Enum entry '" << string_value
                                  << "' not available for '" << param_name
                                  << "' on cam " << i << '\n';
                    }
                } catch (const Spinnaker::Exception& ex) {
                    std::cerr << "[SetParameter] Enum set failed for '"
                              << param_name << "' on cam " << i
                              << ": " << ex.what() << '\n';
                }
                continue;  // node exists as enum — don't fall through to float/int
            }
        }

        // ── Float node (ExposureTime, Gain, Gamma, …) ─────────────────────────
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

        // ── Integer node (Width, Height, OffsetX, …) ──────────────────────────
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

    auto readEnum = [&](const char* node_name) -> std::string {
        CEnumerationPtr p = nm.GetNode(node_name);
        if (IsAvailable(p) && IsReadable(p))
            return std::string(p->GetCurrentEntry()->GetSymbolic().c_str());
        return {};
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

    info.exposure_us   = readFloat("ExposureTime");
    info.gain_db       = readFloat("Gain");
    info.gamma         = readFloat("Gamma");
    info.black_level   = readFloat("BlackLevel");
    info.frame_rate    = readFloat("AcquisitionFrameRate");
    info.exposure_auto = readEnum("ExposureAuto");
    info.gain_auto     = readEnum("GainAuto");

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

void SpinnakerCameraManager::TriggerDiskSave(int32_t camera_id) {
    pending_save_camera_id_.store(camera_id, std::memory_order_release);
}

int32_t SpinnakerCameraManager::GetConnectedCameraCount() const {
    return static_cast<int32_t>(cameras_.size());
}

void SpinnakerCameraManager::GetMaxImageDimensions(int32_t& out_width,
                                                    int32_t& out_height,
                                                    int32_t  fallback_width,
                                                    int32_t  fallback_height) {
    using namespace Spinnaker::GenApi;

    out_width  = 0;
    out_height = 0;

    for (auto& cam : cameras_) {
        if (!cam->IsInitialized()) continue;

        INodeMap& nm = cam->GetNodeMap();
        int32_t   w  = 0;
        int32_t   h  = 0;

        // Priority 1 — SensorWidth/SensorHeight: physical pixel count of the
        // imaging sensor, unaffected by ROI or binning settings.
        {
            CIntegerPtr pw = nm.GetNode("SensorWidth");
            CIntegerPtr ph = nm.GetNode("SensorHeight");
            if (IsAvailable(pw) && IsReadable(pw) &&
                IsAvailable(ph) && IsReadable(ph)) {
                w = static_cast<int32_t>(pw->GetValue());
                h = static_cast<int32_t>(ph->GetValue());
            }
        }

        // Priority 2 — WidthMax/HeightMax: maximum addressable resolution for
        // the current binning setting.  Smaller than SensorWidth if binning>1,
        // but fine as a fallback.
        if (w == 0 || h == 0) {
            CIntegerPtr pw = nm.GetNode("WidthMax");
            CIntegerPtr ph = nm.GetNode("HeightMax");
            if (IsAvailable(pw) && IsReadable(pw) &&
                IsAvailable(ph) && IsReadable(ph)) {
                w = static_cast<int32_t>(pw->GetValue());
                h = static_cast<int32_t>(ph->GetValue());
            }
        }

        // Priority 3 — current Width/Height (may already have ROI applied,
        // but better than nothing).
        if (w == 0 || h == 0) {
            CIntegerPtr pw = nm.GetNode("Width");
            CIntegerPtr ph = nm.GetNode("Height");
            if (IsAvailable(pw) && IsReadable(pw)) w = static_cast<int32_t>(pw->GetValue());
            if (IsAvailable(ph) && IsReadable(ph)) h = static_cast<int32_t>(ph->GetValue());
        }

        if (w > 0) out_width  = std::max(out_width,  w);
        if (h > 0) out_height = std::max(out_height, h);
    }

    // Apply fallback when no cameras were found or nodes were unavailable.
    if (out_width  == 0) out_width  = fallback_width;
    if (out_height == 0) out_height = fallback_height;
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
