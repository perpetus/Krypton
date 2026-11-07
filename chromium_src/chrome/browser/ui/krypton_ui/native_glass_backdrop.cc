// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/native_glass_backdrop.h"

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#undef StoreFence

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

#include "base/environment.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/raw_ref.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/sequence_bound.h"
#include "base/unguessable_token.h"
#include "base/win/object_watcher.h"
#include "base/win/scoped_handle.h"
#include "chrome/browser/ui/krypton_ui/window_session.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "components/viz/common/frame_sinks/blit_request.h"
#include "components/viz/common/frame_sinks/copy_output_request.h"
#include "components/viz/common/gpu/raster_context_provider.h"
#include "content/public/browser/gpu_data_manager.h"
#include "gpu/command_buffer/client/client_shared_image.h"
#include "gpu/command_buffer/client/shared_image_interface.h"
#include "gpu/command_buffer/common/shared_image_info.h"
#include "gpu/config/gpu_info.h"
#include "ui/compositor/compositor.h"
#include "ui/compositor/compositor_observer.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/gpu_memory_buffer_handle.h"
#include "ui/views/widget/widget.h"

namespace krypton_ui {
namespace {
using Microsoft::WRL::ComPtr;
std::atomic<uint64_t> next_resource{1};
constexpr double kMaxSourcePixels = 4 * 1024 * 1024;

// One shared texture per requested geometry, reused for every copy. Fence
// values advance per frame; nothing is allocated or imported per frame.
class GlassFrame : public base::RefCountedThreadSafe<GlassFrame>,
                   public base::win::ObjectWatcher::Delegate {
 public:
  KrGlassBackdropResource resource{};
  uint64_t revision = 0;
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11Fence> ready, released;
  base::win::ScopedHandle texture_handle, ready_handle, release_handle;
  base::win::ScopedHandle import_handle, release_event;
  scoped_refptr<gpu::ClientSharedImage> image;
  uint64_t value = 0;  // Last fence value signaled as ready.
  // busy: copying, or submitted until the module releases it.
  bool registered = false, signaled = false, busy = false;

  void WatchRelease(base::OnceClosure callback) {
    // Keep the image and its native handles alive even if the browser closes
    // or a new geometry supersedes this frame. No UI wait or polling is used.
    on_release_ = std::move(callback);
    keep_alive_ = this;
    if (FAILED(released->SetEventOnCompletion(value, release_event.Get())) ||
        !watcher_.StartWatchingOnce(release_event.Get(), this))
      LOG(ERROR) << "Glass release fence could not be watched";
  }
  void OnObjectSignaled(HANDLE) override {
    auto keep_alive = std::move(keep_alive_);
    if (on_release_)
      std::move(on_release_).Run();
  }

 private:
  friend class base::RefCountedThreadSafe<GlassFrame>;
  ~GlassFrame() override = default;
  base::win::ObjectWatcher watcher_;
  base::OnceClosure on_release_;
  scoped_refptr<GlassFrame> keep_alive_;
};

// D3D device, allocation and immediate-context calls stay on this sequence.
class GlassBackend {
 public:
  scoped_refptr<GlassFrame> Create(uint64_t adapter,
                                   uint64_t revision,
                                   gfx::Size size,
                                   KrRect bounds) {
    if (!Initialize(adapter))
      return nullptr;
    auto frame = base::MakeRefCounted<GlassFrame>();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = size.width();
    desc.Height = size.height();
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags =
        D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &frame->texture)))
      return nullptr;
    ComPtr<IDXGIResource1> shared;
    HANDLE handle = nullptr;
    if (FAILED(frame->texture.As(&shared)) ||
        FAILED(shared->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            nullptr, &handle)))
      return nullptr;
    frame->texture_handle.Set(handle);
    HANDLE imported = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(),
                         &imported, 0, FALSE, DUPLICATE_SAME_ACCESS))
      return nullptr;
    frame->import_handle.Set(imported);
    if (FAILED(device_->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                    IID_PPV_ARGS(&frame->ready))) ||
        FAILED(device_->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                    IID_PPV_ARGS(&frame->released))))
      return nullptr;
    if (FAILED(frame->ready->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
                                                &handle)))
      return nullptr;
    frame->ready_handle.Set(handle);
    if (FAILED(frame->released->CreateSharedHandle(nullptr, GENERIC_ALL,
                                                   nullptr, &handle)))
      return nullptr;
    frame->release_handle.Set(handle);
    frame->release_event.Set(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!frame->release_event.is_valid())
      return nullptr;
    frame->revision = revision;
    frame->resource = {
        .struct_size = sizeof(KrGlassBackdropResource),
        .id = next_resource++,
        .kind = KR_GLASS_D3D11_BGRA8,
        .width = desc.Width,
        .height = desc.Height,
        .texture = reinterpret_cast<uintptr_t>(frame->texture_handle.Get()),
        .ready_fence = reinterpret_cast<uintptr_t>(frame->ready_handle.Get()),
        .release_fence =
            reinterpret_cast<uintptr_t>(frame->release_handle.Get()),
        .adapter_luid = adapter,
        .bounds = bounds};
    return frame;
  }

  bool Signal(scoped_refptr<GlassFrame> frame, uint64_t value) {
    // Viz's completed blit callback precedes this call, so all writes to the
    // imported destination have finished before the module's GPU wait passes.
    if (!context_ || FAILED(context_->Signal(frame->ready.Get(), value)))
      return false;
    context_->Flush();
    return true;
  }

 private:
  bool Initialize(uint64_t adapter) {
    if (device_ && adapter_ == adapter &&
        SUCCEEDED(device_->GetDeviceRemovedReason()))
      return true;
    device_.Reset();
    context_.Reset();
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
      return false;
    for (UINT i = 0;; ++i) {
      ComPtr<IDXGIAdapter1> candidate;
      if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND)
        return false;
      DXGI_ADAPTER_DESC1 desc{};
      if (FAILED(candidate->GetDesc1(&desc)))
        continue;
      const uint64_t luid = static_cast<uint32_t>(desc.AdapterLuid.LowPart) |
                            (static_cast<uint64_t>(static_cast<uint32_t>(
                                 desc.AdapterLuid.HighPart))
                             << 32);
      if (luid != adapter)
        continue;
      ComPtr<ID3D11Device> device;
      ComPtr<ID3D11DeviceContext> context;
      if (FAILED(D3D11CreateDevice(candidate.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                                   nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &device,
                                   nullptr, &context)) ||
          FAILED(device.As(&device_)) || FAILED(context.As(&context_)))
        return false;
      adapter_ = adapter;
      return true;
    }
  }
  uint64_t adapter_ = 0;
  ComPtr<ID3D11Device5> device_;
  ComPtr<ID3D11DeviceContext4> context_;
};
}  // namespace

class NativeGlassBackdrop::Impl : public ui::CompositorObserver {
 public:
  Impl(BrowserView& view, WindowSession& session)
      : view_(view),
        session_(session),
        backend_(base::ThreadPool::CreateSequencedTaskRunner(
            {base::MayBlock(), base::TaskPriority::USER_VISIBLE})) {}
  ~Impl() override {
    Stop();
    if (base::Environment::Create()->GetVar("KR_GLASS_STATS").value_or("") ==
        "1")
      LOG(INFO) << "Krypton glass host: " << captures_ << " copies, "
                << submitted_ << " accepted, " << released_ << " released, "
                << failures_ << " failed copies, " << mismatches_
                << " adapter mismatches";
  }

  void Stop() {
    weak_factory_.InvalidateWeakPtrs();
    if (compositor_)
      compositor_->RemoveObserver(this);
    compositor_ = nullptr;
    if (frame_ && frame_->registered) {
      if (const auto* api = session_->api())
        api->drop_glass_backdrop(api->self, frame_->resource.id);
    }
    frame_.reset();
    context_.reset();
    active_ = allocating_ = dirty_ = suppress_presentation_ = false;
  }

  void Request(const KrGlassBackdropRequest& request) {
    const auto* api = session_->api();
    if (!api || !KR_HAS_SLOT(api, KrShellUI, drop_glass_backdrop) ||
        !api->register_glass_backdrop || !api->submit_glass_backdrop ||
        !api->drop_glass_backdrop || !request.region_count) {
      Stop();
      return;
    }
    if (!request.regions || request.region_count > 256 ||
        !request.adapter_luid || !std::isfinite(request.scale) ||
        request.scale <= 0 || !std::isfinite(request.sampling_margin) ||
        request.sampling_margin < 0)
      return;
    auto* gpu = content::GpuDataManager::GetInstance();
    const auto info = gpu->GetGPUInfo();
    const auto luid = info.active_gpu().luid;
    const uint64_t compositor_adapter =
        static_cast<uint32_t>(luid.LowPart) |
        (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32);
    if (!gpu->HardwareAccelerationEnabled() ||
        (compositor_adapter && compositor_adapter != request.adapter_luid)) {
      ++mismatches_;
      // Cross-adapter imports can fail in the GPU process. Keep the requested
      // preference and let the module's cached Performance source remain.
      Stop();
      return;
    }
    auto* widget = view_->GetWidget();
    auto* compositor = widget ? widget->GetCompositor() : nullptr;
    if (!compositor || !compositor->root_layer())
      return;
    if (compositor_ != compositor) {
      Stop();
      compositor_ = compositor;
      compositor_->AddObserver(this);
    }
    gfx::RectF region;
    for (size_t i = 0; i < request.region_count; ++i) {
      const auto& r = request.regions[i];
      if (!std::isfinite(r.x) || !std::isfinite(r.y) ||
          !std::isfinite(r.width) || !std::isfinite(r.height) || r.width <= 0 ||
          r.height <= 0)
        return;
      region.Union(gfx::RectF(r.x, r.y, r.width, r.height));
    }
    region.Inset(-request.sampling_margin);
    region.Intersect(gfx::RectF(compositor_->root_layer()->bounds()));
    if (region.IsEmpty()) {
      Stop();
      return;
    }
    const auto area =
        gfx::ToEnclosingRect(gfx::ScaleRect(region, request.scale));
    if (area.IsEmpty())
      return;
    const double downscale = std::min(
        1.0, std::sqrt(kMaxSourcePixels /
                       (static_cast<double>(area.width()) * area.height())));
    const gfx::Size output(
        std::max(1, static_cast<int>(area.width() * downscale)),
        std::max(1, static_cast<int>(area.height() * downscale)));
    if (!active_ || adapter_ != request.adapter_luid || area_ != area ||
        size_ != output) {
      // New geometry retires the texture. An in-flight frame keeps itself
      // alive until the module's release fence arrives.
      if (frame_ && frame_->registered)
        api->drop_glass_backdrop(api->self, frame_->resource.id);
      frame_.reset();
      weak_factory_.InvalidateWeakPtrs();
      allocating_ = false;
      active_ = dirty_ = true;
      adapter_ = request.adapter_luid;
      area_ = area;
      size_ = output;
      bounds_ = {area.x() / request.scale, area.y() / request.scale,
                 area.width() / request.scale, area.height() / request.scale};
    }
    if (revision_ != request.revision) {
      // Same geometry, new revision: keep the texture, but frames must carry
      // the new revision, so copy again.
      revision_ = request.revision;
      if (frame_)
        frame_->revision = revision_;
      dirty_ = true;
    }
    TrySubmit();
    Capture();
  }

  void OnDidPresentCompositorFrame(ui::Compositor*,
                                   uint32_t,
                                   const gfx::PresentationFeedback&) override {
    // A copy request itself asks for a compositor frame. Do not turn that
    // presentation into another copy, which would create an idle render loop.
    if (std::exchange(suppress_presentation_, false))
      return;
    dirty_ = true;
    Capture();
  }
  void OnCompositingShuttingDown(ui::Compositor*) override { Stop(); }

 private:
  void Capture() {
    if (!active_ || !dirty_ || allocating_ || !compositor_ ||
        !compositor_->IsVisible() || (frame_ && frame_->busy))
      return;
    dirty_ = false;
    if (frame_) {
      Copy(frame_);
      return;
    }
    allocating_ = true;
    backend_.AsyncCall(&GlassBackend::Create)
        .WithArgs(adapter_, revision_, size_, bounds_)
        .Then(base::BindOnce(&Impl::Allocated, weak_factory_.GetWeakPtr()));
  }
  void Allocated(scoped_refptr<GlassFrame> frame) {
    allocating_ = false;
    if (!frame || !compositor_ || !active_)
      return;
    context_ =
        compositor_->context_factory()->SharedMainThreadRasterContextProvider();
    if (!context_ || context_->IsLost())
      return;
    auto handle = gfx::GpuMemoryBufferHandle(
        gfx::DXGIHandle(std::move(frame->import_handle)));
    frame->image = context_->SharedImageInterface()->CreateSharedImage(
        {viz::SinglePlaneFormat::kBGRA_8888, size_,
         gfx::ColorSpace::CreateSRGB(),
         gpu::SHARED_IMAGE_USAGE_DISPLAY_WRITE |
             gpu::SHARED_IMAGE_USAGE_DISPLAY_READ,
         "KryptonGlassBackdrop"},
        std::move(handle));
    if (!frame->image)
      return;
    frame->revision = revision_;
    frame_ = frame;
    Copy(frame);
  }
  // Drops the texture from the module. An in-flight frame keeps itself alive
  // until its release fence arrives.
  void Retire() {
    if (frame_ && frame_->registered) {
      if (const auto* api = session_->api())
        api->drop_glass_backdrop(api->self, frame_->resource.id);
    }
    frame_.reset();
  }
  void Copy(scoped_refptr<GlassFrame> frame) {
    if (!context_ || context_->IsLost()) {
      Retire();
      return;
    }
    frame->busy = true;
    ++captures_;
    auto request = std::make_unique<viz::CopyOutputRequest>(
        viz::CopyOutputRequest::ResultFormat::RGBA,
        viz::CopyOutputRequest::ResultDestination::kSharedImage,
        base::BindOnce(&Impl::Copied, weak_factory_.GetWeakPtr(), frame));
    request->set_result_task_runner(
        base::SingleThreadTaskRunner::GetCurrentDefault());
    request->set_source(source_);
    request->set_area(area_);
    request->SetScaleRatio(gfx::Vector2d(area_.width(), area_.height()),
                           gfx::Vector2d(size_.width(), size_.height()));
    request->set_result_selection(gfx::Rect(size_));
    // Request GPU completion notification for this native buffer; this flag
    // performs no CPU readback or mapping. Viz submits the completed GPU blit
    // before returning its shared-image result.
    request->set_blit_request(viz::BlitRequest(
        gfx::Point(), viz::LetterboxingBehavior::kDoNotLetterbox, frame->image,
        context_->SharedImageInterface()->GenVerifiedSyncToken(),
        /*populates_mappable_shared_image=*/true));
    suppress_presentation_ = true;
    compositor_->root_layer()->RequestCopyOfOutput(std::move(request));
  }
  void Copied(scoped_refptr<GlassFrame> frame,
              std::unique_ptr<viz::CopyOutputResult> result) {
    if (frame_ != frame)
      return;
    if (!result || result->IsEmpty()) {
      ++failures_;
      frame->busy = false;
      return;
    }
    backend_.AsyncCall(&GlassBackend::Signal)
        .WithArgs(frame, frame->value + 1)
        .Then(base::BindOnce(
            [](base::WeakPtr<Impl> self, scoped_refptr<GlassFrame> frame,
               bool ready) {
              if (!self || self->frame_ != frame)
                return;
              if (!ready) {
                self->Retire();
                return;
              }
              ++frame->value;
              frame->signaled = true;
              self->TrySubmit();
            },
            weak_factory_.GetWeakPtr(), frame));
  }
  void TrySubmit() {
    auto frame = frame_;
    const auto* api = session_->api();
    if (!frame || !frame->signaled || !api)
      return;
    if (!frame->registered) {
      if (api->register_glass_backdrop(api->self, &frame->resource) != KR_OK)
        return;
      frame->registered = true;
    }
    const KrGlassBackdropFrame submission{sizeof(KrGlassBackdropFrame),
                                          frame->resource.id, frame->revision,
                                          frame->value, frame->value};
    // Import is asynchronous. Its completion repeats the current request,
    // which retries this submission without a timer or Store reconciliation.
    if (api->submit_glass_backdrop(api->self, &submission) != KR_OK)
      return;
    frame->signaled = false;
    ++submitted_;
    frame->WatchRelease(base::BindOnce(
        [](base::WeakPtr<Impl> self, scoped_refptr<GlassFrame> frame) {
          frame->busy = false;
          if (!self || self->frame_ != frame)
            return;
          ++self->released_;
          // The texture is free again; copy only if the page changed.
          self->Capture();
        },
        weak_factory_.GetWeakPtr(), frame));
  }

  const raw_ref<BrowserView> view_;
  const raw_ref<WindowSession> session_;
  raw_ptr<ui::Compositor> compositor_ = nullptr;
  base::SequenceBound<GlassBackend> backend_;
  scoped_refptr<viz::RasterContextProvider> context_;
  scoped_refptr<GlassFrame> frame_;
  base::UnguessableToken source_ = base::UnguessableToken::Create();
  gfx::Rect area_;
  gfx::Size size_;
  KrRect bounds_{};
  uint64_t revision_ = 0, adapter_ = 0;
  uint64_t captures_ = 0, submitted_ = 0, released_ = 0, failures_ = 0,
           mismatches_ = 0;
  bool active_ = false, allocating_ = false, dirty_ = false;
  bool suppress_presentation_ = false;
  base::WeakPtrFactory<Impl> weak_factory_{this};
};

NativeGlassBackdrop::NativeGlassBackdrop(BrowserView& view,
                                         WindowSession& session)
    : impl_(std::make_unique<Impl>(view, session)) {}
NativeGlassBackdrop::~NativeGlassBackdrop() = default;
void NativeGlassBackdrop::Request(const KrGlassBackdropRequest& request) {
  impl_->Request(request);
}
void NativeGlassBackdrop::Stop() {
  impl_->Stop();
}
}  // namespace krypton_ui
