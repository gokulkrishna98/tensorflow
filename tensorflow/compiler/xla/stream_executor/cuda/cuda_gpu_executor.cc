/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/stream_executor/cuda/cuda_gpu_executor.h"

#include <cstdint>
#include <optional>
#include <utility>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#if defined(PLATFORM_WINDOWS)
#include <windows.h>
#define PATH_MAX MAX_PATH
#else
#include <unistd.h>
#endif
#include "absl/functional/any_invocable.h"
#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "tensorflow/compiler/xla/stream_executor/cuda/cuda_diagnostics.h"
#include "tensorflow/compiler/xla/stream_executor/cuda/cuda_driver.h"
#include "tensorflow/compiler/xla/stream_executor/cuda/cuda_event.h"
#include "tensorflow/compiler/xla/stream_executor/cuda/cuda_platform_id.h"
#include "tensorflow/compiler/xla/stream_executor/cuda/cuda_stream.h"
#include "tensorflow/compiler/xla/stream_executor/cuda/cuda_timer.h"
#include "tensorflow/compiler/xla/stream_executor/kernel_cache_config.h"
#include "tensorflow/compiler/xla/stream_executor/lib/env.h"
#include "tensorflow/compiler/xla/stream_executor/lib/error.h"
#include "tensorflow/compiler/xla/stream_executor/lib/initialize.h"
#include "tensorflow/compiler/xla/stream_executor/lib/process_state.h"
#include "tensorflow/compiler/xla/stream_executor/lib/statusor.h"
#include "tensorflow/compiler/xla/stream_executor/platform.h"
#include "tensorflow/compiler/xla/stream_executor/platform/logging.h"
#include "tensorflow/compiler/xla/stream_executor/platform/port.h"
#include "tensorflow/compiler/xla/stream_executor/plugin_registry.h"
#include "tensorflow/compiler/xla/stream_executor/stream.h"
#include "tensorflow/compiler/xla/stream_executor/stream_executor_internal.h"
#include "tensorflow/compiler/xla/stream_executor/stream_executor_pimpl.h"
#include "tensorflow/compiler/xla/stream_executor/timer.h"
#include "tensorflow/tsl/platform/numbers.h"

// LOG(ERROR) uses a const named ERROR, so a macro with the same name is
// always unwanted. This happens on Windows that defines such a macro.
#undef ERROR

#ifdef PLATFORMS_GPUS_CUDA_DYNAMIC_LIBCUDA_DYNAMIC_LIBCUDA_H_
#error \
    "No driver calls in this file, wrap driver functionality in cuda_driver.cc."
#endif

#ifdef __CUDA_RUNTIME_H__
#error \
    "CUDA runtime being included into CUDA GPU executor; should be driver only."
#endif

extern bool FLAGS_check_gpu_leaks;
bool FLAGS_prefer_cubin_to_ptx = true;

namespace stream_executor {
namespace gpu {

// Hook that can be used to CUBIN-ate PTX before it is loaded into the driver.
// It has been observed that loading both PTX and cubins into the driver library
// can cause it to crash, but loading only CUBINs avoids those crashes;
// therefore, it's useful to have this hook to hack in uniform CUBIN-ation of
// PTX code.
//
// As this is an implementation-detail workaround, the usage is to declare this
// variable with extern linkage and populate it from another translation unit.
std::function<std::string(const std::string&)> g_cubinate;

static GpuEvent* AsGpuEvent(Event* event) {
  DCHECK(event != nullptr);
  return static_cast<GpuEvent*>(event->implementation());
}

// Given a platform-independent timer datatype, returns the internal CUDA
// platform implementation pointer.
static GpuTimer* AsGpuTimer(Timer* timer) {
  DCHECK(timer != nullptr);
  return static_cast<GpuTimer*>(timer->implementation());
}

// Given const GPU memory, returns a libcuda device pointer datatype, suitable
// for passing directly to libcuda APIs.
//
// N.B. we must lose constness in order to pass a suitable type to the existing
// libcuda APIs, so the caller should take care to only pass the result of const
// GPU memory conversions to libcuda functions which will honor constness.
static CUdeviceptr AsCudaDevicePtr(const DeviceMemoryBase& gpu_mem) {
  return reinterpret_cast<CUdeviceptr>(gpu_mem.opaque());
}

// See description on const version above.
static CUdeviceptr AsCudaDevicePtr(DeviceMemoryBase* gpu_mem) {
  return AsCudaDevicePtr(*gpu_mem);
}

GpuContext* ExtractGpuContext(GpuExecutor* cuda_exec) {
  CHECK(cuda_exec != nullptr);
  return cuda_exec->gpu_context();
}

GpuExecutor::~GpuExecutor() {
  CHECK(kernel_to_gpu_binary_.empty()) << "GpuExecutor has live kernels.";
  CHECK(gpu_binary_to_module_.empty()) << "GpuExecutor has loaded modules.";
  if (context_ != nullptr) {
    GpuDriver::DestroyContext(context_);
  }
}

tsl::Status GpuExecutor::Init(int device_ordinal,
                              DeviceOptions device_options) {
  device_ordinal_ = device_ordinal;

  auto status = GpuDriver::Init();
  if (!status.ok()) {
    return status;
  }

  status = GpuDriver::GetDevice(device_ordinal_, &device_);
  if (!status.ok()) {
    return status;
  }

  status = GpuDriver::CreateContext(device_ordinal_, device_, device_options,
                                    &context_);
  if (!status.ok()) {
    return status;
  }

  return GpuDriver::GetComputeCapability(&cc_major_, &cc_minor_, device_);
}

bool GpuExecutor::FindOnDiskForComputeCapability(
    absl::string_view filename, absl::string_view canonical_suffix,
    std::string* found_filename) const {
  if (cc_major_ == 0 && cc_minor_ == 0) {
    return false;
  }

  std::string cc_specific =
      absl::StrCat(filename, ".cc", cc_major_, cc_minor_, canonical_suffix);
  if (port::FileExists(cc_specific).ok()) {
    VLOG(2) << "found compute-capability-specific file, using that: "
            << cc_specific;
    *found_filename = cc_specific;
    return true;
  }

  VLOG(2) << "could not find compute-capability specific file at: "
          << cc_specific;
  if (port::FileExists(std::string(filename)).ok()) {
    *found_filename = std::string(filename);
    return true;
  }

  return false;
}

bool GpuExecutor::FindOnDiskForISAVersion(absl::string_view filename,
                                          absl::string_view canonical_suffix,
                                          std::string* found_filename) const {
  LOG(ERROR)
      << "Feature not supported on CUDA platform (FindOnDiskForISAVersion)";
  return false;
}
// Returns the path to the running executable.
// N.B. Derived from //knowledge/smalltalk/background_kb.cc
// Arg: strip_exe: if true, remove the name of the executable itself from the
//                 returned string. Example: calling this from /usr/bin/foo
//                 would return /usr/bin.
static std::string GetBinaryDir(bool strip_exe) {
  std::string exe_path = port::GetExecutablePath();
  if (strip_exe) {
    // The exe is the last component of the path, so remove one component.
    std::vector<std::string> components = absl::StrSplit(exe_path, '/');
    components.pop_back();
    return absl::StrJoin(components, "/");
  }
  return exe_path;
}

tsl::Status GpuExecutor::LoadModuleFromCuBin(const char* cubin,
                                             CUmodule* module) {
  uint64_t module_refcount;
  std::tie(*module, module_refcount) = gpu_binary_to_module_[cubin];

  if (*module == nullptr) {
    TF_RETURN_IF_ERROR(GpuDriver::LoadCubin(context_, cubin, module));
    module_refcount = 1;
    VLOG(3) << "Loaded CUBIN " << static_cast<const void*>(cubin)
            << " as module " << *module;
  } else {
    ++module_refcount;
    VLOG(3) << "CUBIN " << static_cast<const void*>(cubin)
            << " is already loaded as module " << *module;
  }
  gpu_binary_to_module_[cubin] = {*module, module_refcount};
  return ::tsl::OkStatus();
}

tsl::Status GpuExecutor::LoadModuleFromPtx(const char* ptx, CUmodule* module) {
  uint64_t module_refcount;
  std::tie(*module, module_refcount) = gpu_binary_to_module_[ptx];

  if (*module == nullptr) {
    TF_RETURN_IF_ERROR(GpuDriver::LoadPtx(context_, ptx, module));
    VLOG(3) << "Loaded PTX " << static_cast<const void*>(ptx) << " as module "
            << *module;
    module_refcount = 1;
  } else {
    ++module_refcount;
    VLOG(3) << "PTX " << static_cast<const void*>(ptx)
            << " is already loaded as module " << module;
  }
  gpu_binary_to_module_[ptx] = {*module, module_refcount};
  return ::tsl::OkStatus();
}

tsl::Status GpuExecutor::LoadModuleFromHsaco(const char* hsaco,
                                             CUmodule* module) {
  return tsl::errors::Internal(
      "Feature not supported on CUDA platform (LoadModuleFromHsaco)");
}

tsl::Status GpuExecutor::GetKernel(const MultiKernelLoaderSpec& spec,
                                   KernelBase* kernel) {
  GpuKernel* cuda_kernel = AsGpuKernel(kernel);
  CUmodule module;
  const std::string* kernelname;

  VLOG(3) << "GetKernel on kernel " << kernel << " : " << kernel->name();

  if (spec.has_cuda_cubin_in_memory()) {
    absl::MutexLock lock{&in_memory_modules_mu_};
    kernelname = &spec.cuda_cubin_in_memory().kernelname();
    const char* cubin = spec.cuda_cubin_in_memory().bytes();
    TF_RETURN_IF_ERROR(LoadModuleFromCuBin(cubin, &module));
    kernel_to_gpu_binary_[kernel] = cubin;
  } else if (spec.has_cuda_ptx_in_memory()) {
    kernelname = &spec.cuda_ptx_in_memory().kernelname();

    if (cc_major_ == 0 && cc_minor_ == 0) {
      return tsl::errors::Internal("Compute capability not set");
    }

    const char* ptx = spec.cuda_ptx_in_memory().text(cc_major_, cc_minor_);
    if (ptx == nullptr) {
      ptx = spec.cuda_ptx_in_memory().default_text();
    }
    if (ptx == nullptr) {
      LOG(FATAL) << "Loader spec has no ptx for kernel " << *kernelname;
    }

    absl::MutexLock lock{&in_memory_modules_mu_};
    TF_RETURN_IF_ERROR(LoadModuleFromPtx(ptx, &module));
    kernel_to_gpu_binary_[kernel] = ptx;
  } else {
    return tsl::errors::Internal("No method of loading CUDA kernel provided");
  }
  VLOG(2) << "getting function " << *kernelname << " from module " << module;
  if (!GpuDriver::GetModuleFunction(context_, module, kernelname->c_str(),
                                    cuda_kernel->gpu_function_ptr())) {
    return tsl::errors::Internal("Could not find the corresponding function");
  }

  // We have to trust the kernel loader spec arity because there doesn't appear
  // to be a way to reflect on the number of expected arguments w/the CUDA API.
  cuda_kernel->set_arity(spec.arity());

  KernelMetadata kernel_metadata;
  TF_RETURN_IF_ERROR(GetKernelMetadata(cuda_kernel, &kernel_metadata));
  kernel->set_metadata(kernel_metadata);
  kernel->set_name(*kernelname);
  return ::tsl::OkStatus();
}

bool GpuExecutor::UnloadGpuBinary(const void* gpu_binary) {
  auto module_it = gpu_binary_to_module_.find(gpu_binary);
  if (gpu_binary_to_module_.end() == module_it) {
    VLOG(3) << "No loaded CUDA module for " << gpu_binary;
    return false;
  }
  auto& module = module_it->second.first;
  auto& refcount = module_it->second.second;
  VLOG(3) << "Found CUDA module " << module << " with refcount " << refcount;
  if (--refcount == 0) {
    VLOG(3) << "Unloading CUDA module " << module;
    GpuDriver::UnloadModule(context_, module);
    gpu_binary_to_module_.erase(module_it);
  }
  return true;
}

void GpuExecutor::UnloadKernel(const KernelBase* kernel) {
  VLOG(3) << "Unloading kernel " << kernel << " : " << kernel->name();

  absl::MutexLock lock{&in_memory_modules_mu_};
  auto gpu_binary_it = kernel_to_gpu_binary_.find(kernel);
  if (kernel_to_gpu_binary_.end() == gpu_binary_it) {
    VLOG(3) << "Kernel " << kernel << " : " << kernel->name()
            << " has never been loaded.";
    return;  // We've never seen this kernel.
  }
  VLOG(3) << "Kernel " << kernel << " : " << kernel->name()
          << " has loaded GPU code " << gpu_binary_it->second;
  UnloadGpuBinary(gpu_binary_it->second);
  kernel_to_gpu_binary_.erase(gpu_binary_it);
}

tsl::Status GpuExecutor::LoadModule(const MultiModuleLoaderSpec& spec,
                                    ModuleHandle* module_handle) {
  // In GpuExecutor we store the pointer to the GPU binary (PTX or CUBIN) as
  // ModuleHandle::id().
  CUmodule cu_module;
  if (spec.has_cuda_cubin_in_memory()) {
    absl::MutexLock lock{&in_memory_modules_mu_};
    TF_RETURN_IF_ERROR(LoadModuleFromCuBin(
        reinterpret_cast<const char*>(spec.cuda_cubin_in_memory().data()),
        &cu_module));
    *module_handle = ModuleHandle(const_cast<void*>(
        static_cast<const void*>(spec.cuda_cubin_in_memory().data())));
    return ::tsl::OkStatus();
  } else if (spec.has_cuda_ptx_in_memory()) {
    if (cc_major_ == 0 && cc_minor_ == 0) {
      return tsl::errors::Internal("Compute capability not set");
    }

    if (!spec.cuda_ptx_in_memory()) {
      return tsl::errors::Internal("PTX not found in spec");
    }

    absl::MutexLock lock{&in_memory_modules_mu_};
    TF_RETURN_IF_ERROR(
        LoadModuleFromPtx(spec.cuda_ptx_in_memory(), &cu_module));
    *module_handle = ModuleHandle(
        const_cast<void*>(static_cast<const void*>(spec.cuda_ptx_in_memory())));
    return ::tsl::OkStatus();
  }
  return tsl::errors::Internal("No method of loading CUDA module provided");
}

bool GpuExecutor::UnloadModule(ModuleHandle module_handle) {
  const char* gpu_binary = reinterpret_cast<const char*>(module_handle.id());
  absl::MutexLock lock{&in_memory_modules_mu_};
  return UnloadGpuBinary(gpu_binary);
}

namespace {
absl::uint128 Fingerprint128(const absl::string_view s) {
  auto fp = tsl::Fingerprint128(s);
  return absl::MakeUint128(fp.high64, fp.low64);
}

int fpus_per_core(int cc_major, int cc_minor) {
  // Source:
  // https://docs.nvidia.com/cuda/cuda-c-programming-guide/index.html#arithmetic-instructions
  int n = 128;          // 5.x, 6.1, 6.2, 8.6, 9.0 -> 128.
  if (cc_major == 3) {  // 3.x -> 192.
    n = 192;
  } else if ((cc_major == 6 && cc_minor == 0) || (cc_major == 7) ||
             (cc_major == 8 && cc_minor == 0)) {
    n = 64;  // 6.0, 7.x, 8.0 -> 64.
  }
  return n;
}

}  // namespace

tsl::StatusOr<std::shared_ptr<DeviceMemoryBase>>
GpuExecutor::CreateOrShareConstant(Stream* stream,
                                   const std::vector<uint8_t>& content) {
  absl::MutexLock lock{&shared_constants_mu_};
  // We assume all constants are uniquely identified by this hash. In the
  // (highly unlikely) event of a hash collision, the program will likely crash
  // (because the cached constant that will be returned by mistake is unlikely
  // to have the correct size).
  absl::uint128 fingerprint = Fingerprint128(absl::string_view(
      reinterpret_cast<const char*>(content.data()), content.size()));
  // Must insert nullptr first to get an iterator to the insertion point.
  auto insert_result = shared_constants_.insert(
      {fingerprint, std::weak_ptr<DeviceMemoryBase>()});
  auto it = insert_result.first;
  bool was_already_in_cache = !insert_result.second;
  std::shared_ptr<DeviceMemoryBase> shared_constant;

  if (was_already_in_cache) {
    shared_constant = it->second.lock();
  }

  if (shared_constant == nullptr) {
    // Either the constant wasn't found in the cache, or it was but its
    // weak_ptr had expired.
    DeviceMemoryBase* new_constant =
        new DeviceMemoryBase(Allocate(content.size(), /*memory_space=*/0));
    if (new_constant->opaque() == nullptr) {
      return tsl::errors::Internal(absl::StrFormat(
          "Failed to allocate %d bytes for new constant", content.size()));
    }

    tsl::Status status =
        stream->ThenMemcpy(new_constant, content.data(), content.size())
            .BlockHostUntilDone();
    if (!status.ok()) {
      Deallocate(new_constant);
      status.Update(tsl::errors::Internal(absl::StrFormat(
          "Memcpy to device address %p failed", new_constant->opaque())));
      return status;
    }

    // Capturing 'this' in the custom deleter means this executor must
    // outlive all shared uses of this constant.
    shared_constant = std::shared_ptr<DeviceMemoryBase>(
        new_constant, [this](DeviceMemoryBase* p) {
          Deallocate(p);
          delete p;
        });
    it->second = std::weak_ptr<DeviceMemoryBase>(shared_constant);
  }

  return shared_constant;
}

tsl::Status GpuExecutor::GetKernelMetadata(GpuKernel* cuda_kernel,
                                           KernelMetadata* kernel_metadata) {
  int value;
  TF_RETURN_IF_ERROR(GpuDriver::FuncGetAttribute(
      CU_FUNC_ATTRIBUTE_NUM_REGS, *cuda_kernel->gpu_function_ptr(), &value));
  kernel_metadata->set_registers_per_thread(value);

  TF_RETURN_IF_ERROR(
      GpuDriver::FuncGetAttribute(CU_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES,
                                  *cuda_kernel->gpu_function_ptr(), &value));
  kernel_metadata->set_shared_memory_bytes(value);
  return ::tsl::OkStatus();
}

tsl::Status GpuExecutor::Launch(Stream* stream, const ThreadDim& thread_dims,
                                const BlockDim& block_dims,
                                const KernelBase& kernel,
                                const KernelArgsArrayBase& args) {
  CHECK_EQ(kernel.Arity() + (args.number_of_shared_bytes() > 0),
           args.number_of_arguments());
  CUstream custream = AsGpuStreamValue(stream);
  const GpuKernel* cuda_kernel = AsGpuKernel(&kernel);
  CUfunction cufunc = cuda_kernel->AsGpuFunctionHandle();

  // Only perform/print the occupancy check once.  Even just checking to see
  // whether we've done an occupancy check on this kernel before isn't free
  // (because we have to synchronize), so we only do this at -v 2+.
  if (VLOG_IS_ON(2)) {
    absl::MutexLock lock(&launched_kernels_mu_);
    if (!launched_kernels_.count(cufunc)) {
      VlogOccupancyInfo(kernel, thread_dims, block_dims);
      // TODO(rspringer): Remove elements from launched_kernels_...if we ever
      // expose a kernel/module deallocation method.
      launched_kernels_.insert(cufunc);
    }
  }

  if (cuda_kernel->GetPreferredCacheConfig() !=
      KernelCacheConfig::kNoPreference) {
    TF_RETURN_IF_ERROR(GpuDriver::FuncSetCacheConfig(
        cufunc, cuda_kernel->GetGpuCacheConfig()));
  }

  void** kernel_params = const_cast<void**>(args.argument_addresses().data());

  return GpuDriver::LaunchKernel(context_, kernel.name(), cufunc, block_dims.x,
                                 block_dims.y, block_dims.z, thread_dims.x,
                                 thread_dims.y, thread_dims.z,
                                 args.number_of_shared_bytes(), custream,
                                 kernel_params, nullptr /* = extra */);
}

// This is a non-essential operation; if there's a failure, proceed without
// logging an error. It's nearly certain that in case of failures, we'd never
// get here in the first place; these are very low-impact routines.
void GpuExecutor::VlogOccupancyInfo(const KernelBase& kernel,
                                    const ThreadDim& thread_dims,
                                    const BlockDim& block_dims) {
  VLOG(2) << "Computing kernel occupancy for kernel "
          << kernel.demangled_name();
  VLOG(2) << "Thread dimensions (" << thread_dims.x << ", " << thread_dims.y
          << ", " << thread_dims.z << ")";

  int regs_per_thread;
  if (!kernel.metadata().registers_per_thread(&regs_per_thread)) {
    return;
  }

  int smem_per_block;
  if (!kernel.metadata().shared_memory_bytes(&smem_per_block)) {
    return;
  }

  const DeviceDescription& device_description =
      kernel.parent()->GetDeviceDescription();

  const GpuKernel* cuda_kernel = AsGpuKernel(&kernel);
  CUfunction cufunc = cuda_kernel->AsGpuFunctionHandle();

  int blocks_per_sm = CalculateOccupancy(device_description, regs_per_thread,
                                         smem_per_block, thread_dims, cufunc);
  VLOG(2) << "Resident blocks per SM is " << blocks_per_sm;

  int suggested_threads =
      CompareOccupancy(&blocks_per_sm, device_description, regs_per_thread,
                       smem_per_block, thread_dims, cufunc);
  if (suggested_threads != 0) {
    VLOG(2) << "The cuda occupancy calculator recommends using "
            << suggested_threads
            << " threads per block to achieve an occupancy of " << blocks_per_sm
            << " blocks per SM.";
  }
}

// Compute and return maximum blocks per core (occupancy) based on the
// device description, some kernel characteristics and the number of threads per
// block.  If unable to compute occupancy, zero is returned.
int GpuExecutor::CalculateOccupancy(const DeviceDescription& device_description,
                                    uint64_t registers_per_thread,
                                    uint64_t shared_memory_per_block,
                                    const ThreadDim& thread_dims,
                                    CUfunction func) {
  int suggested_blocks = 0;
  int suggested_threads = 0;
  CUresult err = cuOccupancyMaxPotentialBlockSize(
      &suggested_blocks, &suggested_threads, func, nullptr,
      shared_memory_per_block, 0);
  CHECK_EQ(err, CUDA_SUCCESS);
  return suggested_blocks;
}

// Compute and return the suggested thread count to achieve ideal occupancy.
// If the provided thread dimensions match this number, zero is returned.
int GpuExecutor::CompareOccupancy(int* initial_blocks,
                                  const DeviceDescription& device_description,
                                  uint64_t registers_per_thread,
                                  uint64_t shared_memory_per_block,
                                  const ThreadDim& thread_dims,
                                  CUfunction func) {
  int suggested_blocks = 0;
  int suggested_threads = 0;
  CUresult err = cuOccupancyMaxPotentialBlockSize(
      &suggested_blocks, &suggested_threads, func, nullptr,
      shared_memory_per_block, 0);
  CHECK_EQ(err, CUDA_SUCCESS);
  if (suggested_blocks > *initial_blocks) {
    *initial_blocks = suggested_blocks;
    return suggested_threads;
  } else {
    return 0;
  }
}

DeviceMemoryBase GpuExecutor::Allocate(uint64_t size, int64_t memory_space) {
  CHECK_EQ(memory_space, 0);
  return DeviceMemoryBase(GpuDriver::DeviceAllocate(context_, size), size);
}

void* GpuExecutor::GetSubBuffer(DeviceMemoryBase* mem, uint64_t offset_bytes,
                                uint64_t size_bytes) {
  // offset and size are in bytes, so char* works as the pointer type.
  return reinterpret_cast<char*>(mem->opaque()) + offset_bytes;
}

void GpuExecutor::Deallocate(DeviceMemoryBase* mem) {
  GpuDriver::DeviceDeallocate(context_, mem->opaque());
}

bool GpuExecutor::HostMemoryRegister(void* location, uint64_t size) {
  if (location == nullptr || size == 0) {
    LOG(WARNING) << "attempting to register null or zero-sized memory: "
                 << location << "; size " << size;
  }
  VLOG(2) << "registering " << location << " size " << size;
  return GpuDriver::HostRegister(context_, location, size);
}

bool GpuExecutor::HostMemoryUnregister(void* location) {
  VLOG(2) << "unregistering " << location;
  return GpuDriver::HostUnregister(context_, location);
}

bool GpuExecutor::SynchronizeAllActivity() {
  return GpuDriver::SynchronizeContext(context_);
}

tsl::Status GpuExecutor::SynchronousMemZero(DeviceMemoryBase* location,
                                            uint64_t size) {
  if (reinterpret_cast<uintptr_t>(location->opaque()) % 4 == 0 &&
      size % 4 == 0) {
    return GpuDriver::SynchronousMemsetUint32(
        context_, AsCudaDevicePtr(location), 0x0, size / 4);
  }
  return GpuDriver::SynchronousMemsetUint8(context_, AsCudaDevicePtr(location),
                                           0x0, size);
}

tsl::Status GpuExecutor::SynchronousMemSet(DeviceMemoryBase* location,
                                           int value, uint64_t size) {
  if (reinterpret_cast<uintptr_t>(location->opaque()) % 4 == 0 &&
      size % 4 == 0) {
    // cudaMemset reinterprets "value" as a uint8_t.
    uint8_t byte_value = static_cast<uint8_t>(value);
    uint32_t pattern = (byte_value << 24) | (byte_value << 16) |
                       (byte_value << 8) | byte_value;
    return GpuDriver::SynchronousMemsetUint32(
        context_, AsCudaDevicePtr(location), pattern, size / 4);
  }
  return GpuDriver::SynchronousMemsetUint8(context_, AsCudaDevicePtr(location),
                                           value, size);
}

tsl::Status GpuExecutor::SynchronousMemcpy(DeviceMemoryBase* gpu_dst,
                                           const void* host_src,
                                           uint64_t size) {
  return GpuDriver::SynchronousMemcpyH2D(context_, AsCudaDevicePtr(gpu_dst),
                                         host_src, size);
}

tsl::Status GpuExecutor::SynchronousMemcpy(void* host_dst,
                                           const DeviceMemoryBase& gpu_src,
                                           uint64_t size) {
  return GpuDriver::SynchronousMemcpyD2H(context_, host_dst,
                                         AsCudaDevicePtr(gpu_src), size);
}

tsl::Status GpuExecutor::SynchronousMemcpyDeviceToDevice(
    DeviceMemoryBase* gpu_dst, const DeviceMemoryBase& gpu_src, uint64_t size) {
  return GpuDriver::SynchronousMemcpyD2D(context_, AsCudaDevicePtr(gpu_dst),
                                         AsCudaDevicePtr(gpu_src), size);
}

tsl::Status GpuExecutor::MemZero(Stream* stream, DeviceMemoryBase* location,
                                 uint64_t size) {
  if (reinterpret_cast<uintptr_t>(location->opaque()) % 4 == 0 &&
      size % 4 == 0) {
    return Memset32(stream, location, 0x0, size);
  } else {
    return Memset(stream, location, 0x0, size);
  }
}

tsl::Status GpuExecutor::Memset(Stream* stream, DeviceMemoryBase* location,
                                uint8_t pattern, uint64_t size) {
  VLOG(2) << "enqueueing memset8 operation onto stream " << stream
          << " at location " << location << " with size " << size
          << " and pattern " << std::hex << pattern;
  return GpuDriver::AsynchronousMemsetUint8(context_, AsCudaDevicePtr(location),
                                            pattern, size,
                                            AsGpuStreamValue(stream));
}

tsl::Status GpuExecutor::Memset32(Stream* stream, DeviceMemoryBase* location,
                                  uint32_t pattern, uint64_t size) {
  VLOG(2) << "enqueueing memset32 operation onto stream " << stream
          << " at location " << location << " with size " << size
          << " and pattern " << std::hex << pattern;
  CHECK(reinterpret_cast<uintptr_t>(location->opaque()) % 4 == 0 &&
        size % 4 == 0);
  return GpuDriver::AsynchronousMemsetUint32(
      context_, AsCudaDevicePtr(location), pattern, size / 4,
      AsGpuStreamValue(stream));
}

bool GpuExecutor::Memcpy(Stream* stream, void* host_dst,
                         const DeviceMemoryBase& gpu_src, uint64_t size) {
  return GpuDriver::AsynchronousMemcpyD2H(context_, host_dst,
                                          AsCudaDevicePtr(gpu_src), size,
                                          AsGpuStreamValue(stream));
}

bool GpuExecutor::Memcpy(Stream* stream, DeviceMemoryBase* gpu_dst,
                         const void* host_src, uint64_t size) {
  return GpuDriver::AsynchronousMemcpyH2D(context_, AsCudaDevicePtr(gpu_dst),
                                          host_src, size,
                                          AsGpuStreamValue(stream));
}

bool GpuExecutor::MemcpyDeviceToDevice(Stream* stream,
                                       DeviceMemoryBase* gpu_dst,
                                       const DeviceMemoryBase& gpu_src,
                                       uint64_t size) {
  return GpuDriver::AsynchronousMemcpyD2D(context_, AsCudaDevicePtr(gpu_dst),
                                          AsCudaDevicePtr(gpu_src), size,
                                          AsGpuStreamValue(stream));
}

bool GpuExecutor::HostCallback(Stream* stream,
                               absl::AnyInvocable<tsl::Status() &&> callback) {
  auto callback_ptr =
      new absl::AnyInvocable<void() &&>([cb = std::move(callback)]() mutable {
        tsl::Status s = std::move(cb)();
        if (!s.ok()) {
          LOG(WARNING) << "Host callback failed: " << s;
        }
      });
  return GpuDriver::AddStreamCallback(context_, AsGpuStreamValue(stream),
                                      InternalHostCallback, callback_ptr);
}

/* static */ void GpuExecutor::InternalHostCallback(CUstream stream,
                                                    CUresult status,
                                                    void* data) {
  auto* callback = reinterpret_cast<absl::AnyInvocable<void() &&>*>(data);
  std::move (*callback)();
  delete callback;
}

tsl::Status GpuExecutor::AllocateEvent(Event* event) {
  return AsGpuEvent(event)->Init();
}

tsl::Status GpuExecutor::DeallocateEvent(Event* event) {
  return AsGpuEvent(event)->Destroy();
}

tsl::Status GpuExecutor::RecordEvent(Stream* stream, Event* event) {
  return AsGpuEvent(event)->Record(AsGpuStream(stream));
}

tsl::Status GpuExecutor::WaitForEvent(Stream* stream, Event* event) {
  if (GpuDriver::WaitStreamOnEvent(context_, AsGpuStream(stream)->gpu_stream(),
                                   AsGpuEvent(event)->gpu_event())) {
    return ::tsl::OkStatus();
  } else {
    return tsl::Status(
        port::error::INTERNAL,
        absl::StrFormat("error recording waiting for CUDA event on stream %p",
                        stream));
  }
}

Event::Status GpuExecutor::PollForEventStatus(Event* event) {
  return AsGpuEvent(event)->PollForStatus();
}

bool GpuExecutor::AllocateStream(Stream* stream) {
  absl::MutexLock l(&alive_gpu_streams_mu_);
  bool out = AsGpuStream(stream)->Init();
  alive_gpu_streams_[stream->implementation()->GpuStreamHack()] = stream;
  return out;
}

void GpuExecutor::DeallocateStream(Stream* stream) {
  GpuStream* cuda_stream = AsGpuStream(stream);
  absl::MutexLock l(&alive_gpu_streams_mu_);
  alive_gpu_streams_.erase(cuda_stream->GpuStreamHack());
  if (!cuda_stream->IsIdle()) {
    LOG(ERROR) << "Deallocating stream with pending work";
  }
  cuda_stream->Destroy();
}

bool GpuExecutor::AllocateTimer(Timer* timer) {
  return AsGpuTimer(timer)->Init();
}

void GpuExecutor::DeallocateTimer(Timer* timer) {
  AsGpuTimer(timer)->Destroy();
}

bool GpuExecutor::CreateStreamDependency(Stream* dependent, Stream* other) {
  CUevent other_completed_event = *AsGpuStream(other)->completed_event();
  bool ok = GpuDriver::RecordEvent(context_, other_completed_event,
                                   AsGpuStreamValue(other))
                .ok();
  if (!ok) {
    LOG(ERROR) << "failed to record completion event; "
                  "therefore, failed to create inter-stream dependency";
    return false;
  }

  return GpuDriver::WaitStreamOnEvent(context_, AsGpuStreamValue(dependent),
                                      other_completed_event);
}

bool GpuExecutor::StartTimer(Stream* stream, Timer* timer) {
  return AsGpuTimer(timer)->Start(AsGpuStream(stream));
}

bool GpuExecutor::StopTimer(Stream* stream, Timer* timer) {
  return AsGpuTimer(timer)->Stop(AsGpuStream(stream));
}

tsl::Status GpuExecutor::BlockHostUntilDone(Stream* stream) {
  return GpuDriver::SynchronizeStream(context_, AsGpuStreamValue(stream));
}

blas::BlasSupport* GpuExecutor::CreateBlas() {
  PluginRegistry* registry = PluginRegistry::Instance();
  tsl::StatusOr<PluginRegistry::BlasFactory> status =
      registry->GetFactory<PluginRegistry::BlasFactory>(cuda::kCudaPlatformId,
                                                        plugin_config_.blas());
  if (!status.ok()) {
    LOG(ERROR) << "Unable to retrieve BLAS factory: "
               << status.status().error_message();
    return nullptr;
  }

  return status.value()(this);
}

dnn::DnnSupport* GpuExecutor::CreateDnn() {
  PluginRegistry* registry = PluginRegistry::Instance();
  tsl::StatusOr<PluginRegistry::DnnFactory> status =
      registry->GetFactory<PluginRegistry::DnnFactory>(cuda::kCudaPlatformId,
                                                       plugin_config_.dnn());
  if (!status.ok()) {
    LOG(ERROR) << "Unable to retrieve DNN factory: "
               << status.status().error_message();
    return nullptr;
  }

  return status.value()(this);
}

fft::FftSupport* GpuExecutor::CreateFft() {
  PluginRegistry* registry = PluginRegistry::Instance();
  tsl::StatusOr<PluginRegistry::FftFactory> status =
      registry->GetFactory<PluginRegistry::FftFactory>(cuda::kCudaPlatformId,
                                                       plugin_config_.fft());
  if (!status.ok()) {
    LOG(ERROR) << "Unable to retrieve FFT factory: "
               << status.status().error_message();
    return nullptr;
  }

  return status.value()(this);
}

rng::RngSupport* GpuExecutor::CreateRng() {
  PluginRegistry* registry = PluginRegistry::Instance();
  tsl::StatusOr<PluginRegistry::RngFactory> status =
      registry->GetFactory<PluginRegistry::RngFactory>(cuda::kCudaPlatformId,
                                                       plugin_config_.rng());
  if (!status.ok()) {
    LOG(ERROR) << "Unable to retrieve RNG factory: "
               << status.status().error_message();
    return nullptr;
  }

  return status.value()(this);
}

// TODO(rspringer): Remove in b/18544742.
bool GpuExecutor::SupportsDnn() const { return true; }

bool GpuExecutor::CanEnablePeerAccessTo(StreamExecutorInterface* other) {
  GpuExecutor* cuda_other = static_cast<GpuExecutor*>(other);
  return GpuDriver::CanEnablePeerAccess(context_, cuda_other->context_);
}

tsl::Status GpuExecutor::EnablePeerAccessTo(StreamExecutorInterface* other) {
  GpuExecutor* cuda_other = static_cast<GpuExecutor*>(other);
  return GpuDriver::EnablePeerAccess(context_, cuda_other->context_);
}

bool GpuExecutor::DeviceMemoryUsage(int64_t* free, int64_t* total) const {
  return GpuDriver::GetDeviceMemoryInfo(context_, free, total);
}

bool GpuExecutor::GetSymbol(const std::string& symbol_name,
                            ModuleHandle module_handle, void** mem,
                            size_t* bytes) {
  CHECK(static_cast<bool>(module_handle));

  auto lookup_in_module = [&](CUmodule module) {
    CHECK(module != nullptr);
    return GpuDriver::GetModuleSymbol(context_, module, symbol_name.c_str(),
                                      reinterpret_cast<CUdeviceptr*>(mem),
                                      bytes);
  };

  {  // give limited scope to mutex_lock
    absl::MutexLock lock{&in_memory_modules_mu_};
    auto it = gpu_binary_to_module_.find(module_handle.id());
    CHECK(it != gpu_binary_to_module_.end());
    return lookup_in_module(it->second.first);
  }

  LOG(INFO) << "Failed to find symbol: " << symbol_name;
  return false;
}

bool FillBlockDimLimit(GpuDeviceHandle device, BlockDim* block_dim_limit) {
  // The BlockDim name is a mismatch against these GRID_DIM_* queries because
  // we use BlockDims to express the dimensions of blocks within a grid
  // (as opposed to ThreadDim which expresses the dimensions of threads
  // within a block).
  int x, y, z;
  if (!GpuDriver::GetGridLimits(&x, &y, &z, device)) {
    return false;
  }

  block_dim_limit->x = x;
  block_dim_limit->y = y;
  block_dim_limit->z = z;
  return true;
}

bool GpuExecutor::SupportsBlas() const { return true; }

bool GpuExecutor::SupportsFft() const { return true; }

bool GpuExecutor::SupportsRng() const { return true; }

std::unique_ptr<internal::EventInterface>
GpuExecutor::CreateEventImplementation() {
  return std::unique_ptr<internal::EventInterface>(new GpuEvent(this));
}

std::unique_ptr<internal::KernelInterface>
GpuExecutor::CreateKernelImplementation() {
  return std::unique_ptr<internal::KernelInterface>(new GpuKernel());
}

std::unique_ptr<internal::StreamInterface>
GpuExecutor::GetStreamImplementation() {
  return std::unique_ptr<internal::StreamInterface>(new GpuStream(this));
}

std::unique_ptr<internal::TimerInterface>
GpuExecutor::GetTimerImplementation() {
  return std::unique_ptr<internal::TimerInterface>(new GpuTimer(this));
}

void* GpuExecutor::GpuContextHack() { return context_; }

GpuContext* GpuExecutor::gpu_context() { return context_; }

// Attempts to read the NUMA node corresponding to the GPU device's PCI bus out
// of SysFS. Returns -1 if it cannot.
//
// For anything more complicated/prod-focused than this, you'll likely want to
// turn to gsys' topology modeling.
static int TryToReadNumaNode(const std::string& pci_bus_id,
                             int device_ordinal) {
#if defined(__APPLE__)
  LOG(INFO) << "OS X does not support NUMA - returning NUMA node zero";
  return 0;
#elif defined(PLATFORM_WINDOWS)
  // Windows support for NUMA is not currently implemented. Return node 0.
  return 0;
#else
  VLOG(2) << "trying to read NUMA node for device ordinal: " << device_ordinal;
  static const int kUnknownNumaNode = -1;

  if (pci_bus_id.empty()) {
    LOG(INFO) << "no PCI bus ID for device ordinal: " << device_ordinal;
    return kUnknownNumaNode;
  }

  std::string filename =
      absl::StrFormat("/sys/bus/pci/devices/%s/numa_node", pci_bus_id);

  // We have to use fopen/fread here so that the device properties can be
  // populated before InitGoogle procedure has been completed (at which point we
  // could use the file::* utilities).
  FILE* file = fopen(filename.c_str(), "r");
  if (file == nullptr) {
    LOG(INFO) << "could not open file to read NUMA node: " << filename
              << "\nYour kernel may have been built without NUMA support.";
    return kUnknownNumaNode;
  }

  std::string content;
  char buf[32];
  size_t did_read = fread(buf, sizeof(buf[0]), sizeof(buf) - 1, file);
  buf[did_read] = '\0';
  content = buf;

  int32_t value;
  if (absl::SimpleAtoi(content, &value)) {
    if (value < 0) {  // See http://b/18228951 for details on this path.
      LOG(INFO) << "successful NUMA node read from SysFS had negative value ("
                << value
                << "), but there must be at least one NUMA node"
                   ", so returning NUMA node zero."
                   " See more at "
                   "https://github.com/torvalds/linux/blob/v6.0/Documentation/"
                   "ABI/testing/sysfs-bus-pci#L344-L355";
      fclose(file);
      return 0;
    }
    fclose(file);
    return value;
  }

  LOG(WARNING)
      << "could not convert SysFS file contents to integral NUMA node value: "
      << content;

  fclose(file);
  return kUnknownNumaNode;
#endif
}

tsl::StatusOr<std::unique_ptr<DeviceDescription>>
GpuExecutor::CreateDeviceDescription(int device_ordinal) {
  GpuDeviceHandle device;
  auto status = GpuDriver::GetDevice(device_ordinal, &device);
  if (!status.ok()) {
    return status;
  }

  int cc_major;
  int cc_minor;
  status = GpuDriver::GetComputeCapability(&cc_major, &cc_minor, device);
  if (!status.ok()) {
    return status;
  }

  internal::DeviceDescriptionBuilder builder;

  {
    int driver_version = 0;
    (void)GpuDriver::GetDriverVersion(&driver_version);
    std::string augmented_driver_version = absl::StrFormat(
        "%d (%s)", driver_version,
        cuda::DriverVersionStatusToString(Diagnostician::FindDsoVersion()));
    builder.set_driver_version(augmented_driver_version);
  }

  {
    std::string pci_bus_id = GpuDriver::GetPCIBusID(device);

    // Lower the hex characters to match sysfs.
    pci_bus_id = absl::AsciiStrToLower(pci_bus_id);
    builder.set_pci_bus_id(pci_bus_id);

    // Read the NUMA node corresponding to the PCI bus ID out of sysfs.
    int numa_node = TryToReadNumaNode(pci_bus_id, device_ordinal);
    builder.set_numa_node(numa_node);
  }

  {
    builder.set_threads_per_block_limit(
        GpuDriver::GetDeviceAttribute(CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK,
                                      device)
            .value());

    ThreadDim thread_dim_limit;
    thread_dim_limit.x = GpuDriver::GetDeviceAttribute(
                             CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_X, device)
                             .value();
    thread_dim_limit.y = GpuDriver::GetDeviceAttribute(
                             CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Y, device)
                             .value();
    thread_dim_limit.z = GpuDriver::GetDeviceAttribute(
                             CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Z, device)
                             .value();
    builder.set_thread_dim_limit(thread_dim_limit);
  }

  int sm_clock_khz =
      GpuDriver::GetDeviceAttribute(CU_DEVICE_ATTRIBUTE_CLOCK_RATE, device)
          .value();
  builder.set_clock_rate_ghz(static_cast<float>(sm_clock_khz) / 1e6);

  {
    bool ecc_enabled = false;
    (void)GpuDriver::IsEccEnabled(device, &ecc_enabled);
    builder.set_ecc_enabled(ecc_enabled);
  }

  uint64_t device_memory_size = static_cast<uint64_t>(-1);
  (void)GpuDriver::GetDeviceTotalMemory(device, &device_memory_size);
  builder.set_device_memory_size(device_memory_size);

  int64_t l2_cache_bytes =
      GpuDriver::GetDeviceAttribute(CU_DEVICE_ATTRIBUTE_L2_CACHE_SIZE, device)
          .value();
  builder.set_l2_cache_size(l2_cache_bytes);

  tsl::StatusOr<int> mem_clock_khz = GpuDriver::GetDeviceAttribute(
      CU_DEVICE_ATTRIBUTE_MEMORY_CLOCK_RATE, device_ordinal);
  tsl::StatusOr<int> mem_bus_width_bits = GpuDriver::GetDeviceAttribute(
      CU_DEVICE_ATTRIBUTE_GLOBAL_MEMORY_BUS_WIDTH, device_ordinal);
  if (mem_clock_khz.ok() && mem_bus_width_bits.ok()) {
    // Times 2 because HBM is DDR memory; it gets two data bits per each data
    // lane.
    builder.set_memory_bandwidth(2 * int64_t{mem_clock_khz.value()} * 1000 *
                                 int64_t{mem_bus_width_bits.value()} / 8);
  }

  {
    BlockDim block_dim_limit;
    FillBlockDimLimit(device, &block_dim_limit);
    builder.set_block_dim_limit(block_dim_limit);
  }

  {
    std::string device_name;
    TF_RETURN_IF_ERROR(GpuDriver::GetDeviceName(device, &device_name));
    builder.set_name(device_name);
  }

  builder.set_platform_version(
      absl::StrCat("Compute Capability ", cc_major, ".", cc_minor));

  // TODO(leary) should be a way to query this from the driver, but this is
  // unlikely to change for us any time soon.
  builder.set_device_address_bits(64);

  builder.set_device_vendor("NVIDIA Corporation");
  builder.set_cuda_compute_capability(cc_major, cc_minor);
  builder.set_shared_memory_per_core(
      GpuDriver::GetMaxSharedMemoryPerCore(device).value());
  builder.set_shared_memory_per_block(
      GpuDriver::GetMaxSharedMemoryPerBlock(device).value());
  int core_count = GpuDriver::GetMultiprocessorCount(device).value();
  builder.set_core_count(core_count);
  builder.set_fpus_per_core(fpus_per_core(cc_major, cc_minor));
  builder.set_threads_per_core_limit(
      GpuDriver::GetMaxThreadsPerMultiprocessor(device).value());
  builder.set_registers_per_block_limit(
      GpuDriver::GetMaxRegistersPerBlock(device).value());
  builder.set_threads_per_warp(GpuDriver::GetThreadsPerWarp(device).value());
  builder.set_registers_per_core_limit(
      GpuDriver::GetDeviceAttribute(
          CU_DEVICE_ATTRIBUTE_MAX_REGISTERS_PER_MULTIPROCESSOR, device)
          .value());

  auto value_or = [](const auto& status_or, auto default_val) {
    if (status_or.ok()) return *status_or;
    return default_val;
  };

  // It would be better to use the PCI device ID or some other truly unique
  // identifier for the GPU model.  But getting this requires using NVML or
  // other hacks, which we don't have access to in OSS TensorFlow.
  //
  // Alternatively you might be tempted to use GpuDriver::GetDeviceName as a
  // unique identifier, but this is not stable across GPU VBIOS versions.
  //
  // For now, this identifier is good enough.
  builder.set_model_str(absl::StrFormat(
      "sm_%d.%d with %dB RAM, %d cores, %dKHz clock, %dKHz mem clock, %dB L2$",
      cc_major, cc_minor, device_memory_size, core_count, sm_clock_khz,
      value_or(mem_clock_khz, 0), l2_cache_bytes));

  return builder.Build();
}

}  // namespace gpu

}  // namespace stream_executor

REGISTER_MODULE_INITIALIZER(cuda_gpu_executor, {});
