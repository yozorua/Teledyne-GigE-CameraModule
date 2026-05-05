#include "GrpcServer.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
// CameraControlServiceImpl
// ─────────────────────────────────────────────────────────────────────────────

CameraControlServiceImpl::CameraControlServiceImpl(SpinnakerCameraManager& cam_mgr,
                                                   SharedMemoryManager&    shm_mgr)
    : cam_mgr_(cam_mgr), shm_mgr_(shm_mgr) {}

grpc::Status CameraControlServiceImpl::GetSystemState(
    grpc::ServerContext*,
    const camaramodule::Empty*,
    camaramodule::SystemState* resp)
{
    const int32_t count     = cam_mgr_.GetConnectedCameraCount();
    const bool    acquiring = cam_mgr_.IsAcquiring();

    if (count == 0) {
        resp->set_status("ERROR");
    } else if (!acquiring) {
        resp->set_status("IDLE");
    } else {
        // Check whether all cameras are acquiring or just some.
        bool all = true;
        for (int32_t i = 0; i < count; ++i)
            if (!cam_mgr_.IsCameraAcquiring(i)) { all = false; break; }
        resp->set_status(all ? "ACQUIRING" : "PARTIAL");
    }

    resp->set_connected_cameras(count);
    resp->set_current_fps(cam_mgr_.GetCurrentFPS());
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::StartAcquisition(
    grpc::ServerContext*,
    const camaramodule::CameraRequest* req,
    camaramodule::CommandStatus*       resp)
{
    const int32_t cam_id = req->camera_id();
    const bool    ok     = cam_mgr_.StartAcquisition(cam_id);
    resp->set_success(ok);
    if (cam_id == -1)
        resp->set_message(ok ? "Acquisition started on all cameras."
                             : "Failed to start acquisition.");
    else
        resp->set_message(ok ? "Acquisition started on camera " + std::to_string(cam_id) + "."
                             : "Failed to start camera " + std::to_string(cam_id) + ".");
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::StopAcquisition(
    grpc::ServerContext*,
    const camaramodule::CameraRequest* req,
    camaramodule::CommandStatus*       resp)
{
    const int32_t cam_id = req->camera_id();
    const bool    ok     = cam_mgr_.StopAcquisition(cam_id);
    resp->set_success(ok);
    if (cam_id == -1)
        resp->set_message(ok ? "Acquisition stopped on all cameras."
                             : "Failed to stop acquisition.");
    else
        resp->set_message(ok ? "Acquisition stopped on camera " + std::to_string(cam_id) + "."
                             : "Failed to stop camera " + std::to_string(cam_id) + ".");
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::SetParameter(
    grpc::ServerContext*,
    const camaramodule::ParameterRequest* req,
    camaramodule::CommandStatus*          resp)
{
    const bool ok = cam_mgr_.SetParameter(
        req->param_name(), req->float_value(), req->int_value(),
        req->camera_id(), req->string_value());
    resp->set_success(ok);
    resp->set_message(ok ? "Parameter set." : "Parameter not found or not writable.");
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::TriggerDiskSave(
    grpc::ServerContext*,
    const camaramodule::CameraRequest* req,
    camaramodule::CommandStatus*       resp)
{
    const int32_t cam_id = req->camera_id();
    cam_mgr_.TriggerDiskSave(cam_id);
    const std::string target = (cam_id == -1) ? "any camera"
                                              : "camera " + std::to_string(cam_id);
    resp->set_success(true);
    resp->set_message("Disk save queued for next frame from " + target + ".");
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::SetSaveDirectory(
    grpc::ServerContext*,
    const camaramodule::SaveDirectoryRequest* req,
    camaramodule::CommandStatus*              resp)
{
    if (req->path().empty()) {
        resp->set_success(false);
        resp->set_message("Path must not be empty.");
        return grpc::Status::OK;
    }
    cam_mgr_.SetSaveDirectory(req->path());
    resp->set_success(true);
    resp->set_message("Save directory set to: " + req->path());
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetCameraInfo(
    grpc::ServerContext*,
    const camaramodule::CameraRequest* req,
    camaramodule::CameraState*         resp)
{
    CameraInfo info;
    if (!cam_mgr_.GetCameraInfo(req->camera_id(), info)) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "Camera " + std::to_string(req->camera_id()) +
                            " not found or not initialized.");
    }

    resp->set_camera_id(info.camera_id);
    resp->set_model_name(info.model_name);
    resp->set_serial(info.serial);
    resp->set_ip_address(info.ip_address);
    resp->set_width(info.width);
    resp->set_height(info.height);
    resp->set_offset_x(info.offset_x);
    resp->set_offset_y(info.offset_y);
    resp->set_binning_h(info.binning_h);
    resp->set_binning_v(info.binning_v);
    resp->set_exposure_us(info.exposure_us);
    resp->set_gain_db(info.gain_db);
    resp->set_fps(info.fps);
    resp->set_acquiring(info.acquiring);
    resp->set_gamma(info.gamma);
    resp->set_black_level(info.black_level);
    resp->set_frame_rate(info.frame_rate);
    resp->set_exposure_auto(info.exposure_auto);
    resp->set_gain_auto(info.gain_auto);
    resp->set_ev_compensation(info.ev_compensation);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetLatestImageFrame(
    grpc::ServerContext*,
    const camaramodule::FrameRequest* req,
    camaramodule::FrameInfo*          resp)
{
    const int32_t camera_id = req->camera_id();
    const int32_t idx       = shm_mgr_.AcquireLatestFrame(camera_id);

    if (idx < 0) {
        const std::string msg = (camera_id < 0)
            ? "No frame available from any camera."
            : "No frame available from camera " + std::to_string(camera_id) + ".";
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, msg);
    }

    const SharedMemoryHeader* hdr = shm_mgr_.GetHeader();

    resp->set_shared_memory_index(idx);
    resp->set_timestamp(hdr->buffer_timestamp_us[idx]);  // µs since Unix epoch
    resp->set_width(hdr->buffer_width[idx]);    // actual ROI, not the SHM max
    resp->set_height(hdr->buffer_height[idx]);
    resp->set_camera_id(hdr->buffer_camera_id[idx]);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::ReleaseImageFrame(
    grpc::ServerContext*,
    const camaramodule::ReleaseRequest* req,
    camaramodule::CommandStatus*        resp)
{
    shm_mgr_.ReleaseFrame(req->shared_memory_index());
    resp->set_success(true);
    resp->set_message("Frame released.");
    return grpc::Status::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// GrpcServer
// ─────────────────────────────────────────────────────────────────────────────

GrpcServer::GrpcServer(SpinnakerCameraManager& cam_mgr, SharedMemoryManager& shm_mgr)
    : cam_mgr_(cam_mgr), shm_mgr_(shm_mgr) {}

GrpcServer::~GrpcServer() {
    Shutdown();
}

void GrpcServer::Start(const std::string& listen_address) {
    service_ = std::make_unique<CameraControlServiceImpl>(cam_mgr_, shm_mgr_);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
    builder.RegisterService(service_.get());
    builder.AddChannelArgument(GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH, -1);

    server_ = builder.BuildAndStart();
    if (!server_) {
        throw std::runtime_error("Failed to start gRPC server on " + listen_address);
    }

    std::cout << "[GrpcServer] Listening on " << listen_address << '\n';
    server_->Wait();
}

void GrpcServer::Shutdown() {
    if (server_) {
        server_->Shutdown();
        server_.reset();
    }
}
