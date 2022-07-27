/*******************************************************************************
* Copyright 2019-2022 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef SYCL_STREAM_HPP
#define SYCL_STREAM_HPP

#include "common/c_types_map.hpp"
#include "common/primitive_exec_types.hpp"
#include "common/primitive_iface.hpp"
#include "common/stream.hpp"
#include "common/thread_local_storage.hpp"
#include "common/utils.hpp"
#include "gpu/compute/compute_stream.hpp"
#include "gpu/ocl/ocl_utils.hpp"
#include "gpu/profile.hpp"
#include "gpu/sycl/sycl_gpu_engine.hpp"
#include "sycl/profile.hpp"
#include "sycl/sycl_memory_storage.hpp"

#if DNNL_CPU_RUNTIME == DNNL_RUNTIME_SYCL
#include "sycl/sycl_stream_cpu_thunk.hpp"
#include "sycl/sycl_stream_submit_cpu_primitive.hpp"
#endif

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <utility>
#include <CL/cl.h>
#include <CL/sycl.hpp>

namespace dnnl {
namespace impl {
namespace sycl {

struct sycl_stream_t : public gpu::compute::compute_stream_t {
    static status_t create_stream(
            stream_t **stream, engine_t *engine, unsigned flags) {
        std::unique_ptr<sycl_stream_t> sycl_stream(
                new sycl_stream_t(engine, flags));
        if (!sycl_stream) return status::out_of_memory;

        status_t status = sycl_stream->init();
        if (status != status::success) return status;
        *stream = sycl_stream.release();
        return status::success;
    }

    static status_t create_stream(
            stream_t **stream, engine_t *engine, ::sycl::queue &queue) {
        unsigned flags;
        status_t status = sycl_stream_t::init_flags(&flags, queue);
        if (status != status::success) return status;

        std::unique_ptr<sycl_stream_t> sycl_stream(
                new sycl_stream_t(engine, flags, queue));

        status = sycl_stream->init();
        if (status != status::success) return status;

        *stream = sycl_stream.release();
        return status::success;
    }

    status_t wait() override {
        queue_->wait_and_throw();
        return status::success;
    }

    ::sycl::queue &queue() { return *queue_; }

    status_t enqueue_primitive(const primitive_iface_t *prim_iface,
            exec_ctx_t &exec_ctx) override {
        auto execute_func = [&]() {
            status_t status = status::success;
            if (engine()->kind() == engine_kind::cpu) {

#if DNNL_CPU_RUNTIME == DNNL_RUNTIME_SYCL
                auto event = queue_->submit([&](::sycl::handler &cgh) {
                    register_deps(cgh);
                    submit_cpu_primitive(this, prim_iface, exec_ctx, cgh);
                });
                set_deps({event});
#else
                assert(!"not expected");
                return status::runtime_error;
#endif
            } else if (engine()->kind() == engine_kind::gpu) {
                status = prim_iface->execute(exec_ctx);
            } else {
                assert(!"not expected");
            }
            return status;
        };
        status_t status = execute_func();
        return status;
    }

    status_t copy(const memory_storage_t &src, const memory_storage_t &dst,
            size_t size) override {
        if (size == 0) return status::success;
        // TODO: add src and dst sizes check

        const bool host_mem_src = src.engine()->kind() == engine_kind::cpu
                && is_native_runtime(src.engine()->runtime_kind());
        const bool host_mem_dst = dst.engine()->kind() == engine_kind::cpu
                && is_native_runtime(dst.engine()->runtime_kind());

        // Handle cases when GPU runtime is SYCL and CPU runtime is not.
        if (host_mem_src || host_mem_dst) {
            void *src_mapped_ptr;
            void *dst_mapped_ptr;

            CHECK(src.map_data(&src_mapped_ptr, this, size));
            CHECK(dst.map_data(&dst_mapped_ptr, this, size));

            std::memcpy(static_cast<void *>(dst_mapped_ptr),
                    static_cast<const void *>(src_mapped_ptr), size);

            CHECK(src.unmap_data(src_mapped_ptr, this));
            CHECK(dst.unmap_data(dst_mapped_ptr, this));

            return status::success;
        }

        // Handle all other cases.
        auto *sycl_src
                = utils::downcast<const sycl_memory_storage_base_t *>(&src);
        auto *sycl_dst
                = utils::downcast<const sycl_memory_storage_base_t *>(&dst);
        bool usm_src = sycl_src->memory_kind() == memory_kind::usm;
        bool usm_dst = sycl_dst->memory_kind() == memory_kind::usm;
        ::sycl::event e;

        if (usm_src && usm_dst) {
            auto *usm_src
                    = utils::downcast<const sycl_usm_memory_storage_t *>(&src);
            auto *usm_dst
                    = utils::downcast<const sycl_usm_memory_storage_t *>(&dst);
            e = queue_->submit([&](::sycl::handler &cgh) {
                register_deps(cgh);
                cgh.memcpy(usm_dst->usm_ptr(), usm_src->usm_ptr(), size);
            });
        } else if (usm_src && !usm_dst) {
            auto *usm_src
                    = utils::downcast<const sycl_usm_memory_storage_t *>(&src);
            auto *buffer_dst
                    = utils::downcast<const sycl_buffer_memory_storage_t *>(
                            &dst);
            auto &b_dst = buffer_dst->buffer();
            e = queue_->submit([&](::sycl::handler &cgh) {
                register_deps(cgh);
                auto acc_dst
                        = b_dst.get_access<::sycl::access::mode::write>(cgh);
                cgh.copy(usm_src->usm_ptr(), acc_dst);
            });
        } else if (!usm_src && usm_dst) {
            auto *buffer_src
                    = utils::downcast<const sycl_buffer_memory_storage_t *>(
                            &src);
            auto &b_src = buffer_src->buffer();
            auto *usm_dst
                    = utils::downcast<const sycl_usm_memory_storage_t *>(&dst);
            e = queue_->submit([&](::sycl::handler &cgh) {
                register_deps(cgh);
                auto acc_src
                        = b_src.get_access<::sycl::access::mode::read>(cgh);
                cgh.copy(acc_src, usm_dst->usm_ptr());
            });
        } else { // if (!usm_src && !usm_dst)
            assert(!usm_src && !usm_dst && "USM is not supported yet");
            auto *buffer_src
                    = utils::downcast<const sycl_buffer_memory_storage_t *>(
                            &src);
            auto *buffer_dst
                    = utils::downcast<const sycl_buffer_memory_storage_t *>(
                            &dst);
            auto &b_src = buffer_src->buffer();
            auto &b_dst = buffer_dst->buffer();
            e = queue_->submit([&](::sycl::handler &cgh) {
                auto acc_src
                        = b_src.get_access<::sycl::access::mode::read>(cgh);
                auto acc_dst
                        = b_dst.get_access<::sycl::access::mode::write>(cgh);
                register_deps(cgh);
                cgh.copy(acc_src, acc_dst);
            });
        }
        if (gpu::is_profiling_enabled()) register_profiling_event(e);
        set_deps({e});

        return status::success;
    }

    status_t fill(const memory_storage_t &dst, uint8_t pattern,
            size_t size) override {
        auto *sycl_dst
                = utils::downcast<const sycl_memory_storage_base_t *>(&dst);
        bool usm = sycl_dst->memory_kind() == memory_kind::usm;

        ::sycl::event out_event;

        if (usm) {
            auto *usm_dst
                    = utils::downcast<const sycl_usm_memory_storage_t *>(&dst);
            auto dst_ptr = static_cast<uint8_t *>(usm_dst->usm_ptr());
            // Note: we cannot use queue_.fill since it cannot handle
            // events as input
            out_event = queue_->submit([&](::sycl::handler &cgh) {
                register_deps(cgh);
                cgh.memset(dst_ptr, pattern, size);
            });
        } else {
            auto *buffer_dst
                    = utils::downcast<const sycl_buffer_memory_storage_t *>(
                            &dst);
            out_event = queue_->submit([&](::sycl::handler &cgh) {
                // need a u8 accessor to get the proper range
                ::sycl::accessor<uint8_t, 1, ::sycl::access::mode::write,
                        compat::target_device>
                        acc_dst(buffer_dst->buffer(), cgh,
                                ::sycl::range<1>(size), ::sycl::id<1>(0));
                register_deps(cgh);
                cgh.fill(acc_dst, pattern);
            });
        }
        if (gpu::is_profiling_enabled()) register_profiling_event(out_event);
        set_deps({out_event});
        return status::success;
    }

    std::vector<::sycl::event> &get_deps() {
        auto &deps = const_cast<const sycl_stream_t *>(this)->get_deps();
        return const_cast<std::vector<::sycl::event> &>(deps);
    }
    const std::vector<::sycl::event> &get_deps() const {
        static std::vector<::sycl::event> empty_deps;
        return deps_tls_.get(empty_deps);
    }

    void set_deps(const std::vector<::sycl::event> &deps) { get_deps() = deps; }
    void add_dep(const ::sycl::event &dep) { get_deps().push_back(dep); }
    ::sycl::event get_output_event() const {
        // Fast path: if only one event, return it.
        auto &deps = get_deps();
        if (deps.size() == 1) return deps[0];

        // Otherwise, we run a trivial kernel to gather all deps. The
        // dummy task is needed to not get an error related to empty
        // kernel.
        auto e = queue_->submit([&](::sycl::handler &cgh) {
            register_deps(cgh);
            cgh.single_task<class dnnl_dummy_kernel>([]() {});
        });
        return e;
    }
    void register_deps(::sycl::handler &cgh) const {
        cgh.depends_on(get_deps());
    }

    template <::sycl::access_mode mode>
    ::sycl::accessor<uint8_t, 1, mode> get_dummy_accessor(
            ::sycl::handler &cgh) {
        return dummy_buffer_.get_access<mode>(cgh);
    }

protected:
    sycl_stream_t(engine_t *engine, unsigned flags)
        : gpu::compute::compute_stream_t(engine, flags) {}
    sycl_stream_t(engine_t *engine, unsigned flags, ::sycl::queue &queue)
        : gpu::compute::compute_stream_t(engine, flags)
        , queue_(new ::sycl::queue(queue)) {}

    static status_t init_flags(unsigned *flags, ::sycl::queue &queue) {
        *flags = queue.is_in_order() ? stream_flags::in_order
                                     : stream_flags::out_of_order;
        return status::success;
    }

    std::unique_ptr<::sycl::queue> queue_;
    mutable utils::thread_local_storage_t<std::vector<::sycl::event>> deps_tls_;

    // XXX: this is a temporary solution to make sycl_memory_arg_t
    // default constructible.
    buffer_u8_t dummy_buffer_ = buffer_u8_t(1);

private:
    status_t init();
};

} // namespace sycl
} // namespace impl
} // namespace dnnl

#endif
