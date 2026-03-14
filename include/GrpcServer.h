#pragma once

#include <grpcpp/grpcpp.h>
#include "camera_service.grpc.pb.h"

#include "SpinnakerCameraManager.h"
#include "SharedMemoryManager.h"

#include <memory>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// CameraControlServiceImpl
// ─────────────────────────────────────────────────────────────────────────────

class CameraControlServiceImpl final : public camaramodule::CameraControl::Service {
public:
    CameraControlServiceImpl(SpinnakerCameraManager& cam_mgr,
                             SharedMemoryManager&    shm_mgr);

    grpc::Status GetSystemState(grpc::ServerContext*        ctx,
                                const camaramodule::Empty*  req,
                                camaramodule::SystemState*  resp) override;

    grpc::Status StartAcquisition(grpc::ServerContext*              ctx,
                                  const camaramodule::CameraRequest* req,
                                  camaramodule::CommandStatus*       resp) override;

    grpc::Status StopAcquisition(grpc::ServerContext*              ctx,
                                 const camaramodule::CameraRequest* req,
                                 camaramodule::CommandStatus*       resp) override;

    grpc::Status SetParameter(grpc::ServerContext*                   ctx,
                              const camaramodule::ParameterRequest*  req,
                              camaramodule::CommandStatus*           resp) override;

    grpc::Status TriggerDiskSave(grpc::ServerContext*               ctx,
                                 const camaramodule::CameraRequest* req,
                                 camaramodule::CommandStatus*       resp) override;

    grpc::Status SetSaveDirectory(grpc::ServerContext*                      ctx,
                                  const camaramodule::SaveDirectoryRequest* req,
                                  camaramodule::CommandStatus*              resp) override;

    grpc::Status GetCameraInfo(grpc::ServerContext*              ctx,
                               const camaramodule::CameraRequest* req,
                               camaramodule::CameraState*         resp) override;

    grpc::Status GetLatestImageFrame(grpc::ServerContext*              ctx,
                                     const camaramodule::FrameRequest* req,
                                     camaramodule::FrameInfo*          resp) override;

    grpc::Status ReleaseImageFrame(grpc::ServerContext*                ctx,
                                   const camaramodule::ReleaseRequest* req,
                                   camaramodule::CommandStatus*        resp) override;

private:
    SpinnakerCameraManager& cam_mgr_;
    SharedMemoryManager&    shm_mgr_;
};

// ─────────────────────────────────────────────────────────────────────────────
// GrpcServer
// ─────────────────────────────────────────────────────────────────────────────

class GrpcServer {
public:
    GrpcServer(SpinnakerCameraManager& cam_mgr, SharedMemoryManager& shm_mgr);
    ~GrpcServer();

    GrpcServer(const GrpcServer&)            = delete;
    GrpcServer& operator=(const GrpcServer&) = delete;

    /// Blocking call: starts the server and waits until Shutdown() is called.
    void Start(const std::string& listen_address);

    /// Thread-safe; may be called from a signal handler via a flag.
    void Shutdown();

private:
    SpinnakerCameraManager&                   cam_mgr_;
    SharedMemoryManager&                      shm_mgr_;
    std::unique_ptr<CameraControlServiceImpl> service_;
    std::unique_ptr<grpc::Server>             server_;
};
