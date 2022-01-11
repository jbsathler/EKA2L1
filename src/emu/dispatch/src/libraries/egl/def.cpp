/*
 * Copyright (c) 2021 EKA2L1 Team.
 * 
 * This file is part of EKA2L1 project.
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <dispatch/libraries/egl/def.h>
#include <drivers/graphics/graphics.h>

#include <common/log.h>

namespace eka2l1::dispatch {
    egl_context::egl_context()
        : draw_surface_(nullptr)
        , read_surface_(nullptr)
        , associated_thread_uid_(0)
        , dead_pending_(false) {
    }

    egl_surface::egl_surface(epoc::canvas_base *backed_window, epoc::screen *screen, eka2l1::vec2 dim,
        drivers::handle h, egl_config::surface_type surface_type)
        : handle_(h)
        , surface_type_(surface_type)
        , backed_screen_(screen)
        , backed_window_(backed_window)
        , dimension_(dim)
        , associated_thread_uid_(0)
        , dead_pending_(false)
        , bounded_context_(nullptr) {
    }

    void egl_context::free(drivers::graphics_driver *driver, drivers::graphics_command_builder &builder) {
        drivers::command_list retrieved = builder.retrieve_command_list();
        driver->submit_command_list(retrieved);
    }

    egl_controller::egl_controller(drivers::graphics_driver *driver)
        : driver_(driver)
        , es1_shaderman_(driver) {
    }

    egl_controller::~egl_controller() {
        drivers::graphics_command_builder cmd_builder;
        bool freed_once = false;

        for (auto &ctx: contexts_) {
            if (ctx) {
                ctx->free(driver_, cmd_builder);
                freed_once = true;
            }
        }

        for (auto &surface: dsurfaces_) {
            if (surface) {
                cmd_builder.destroy_bitmap(surface->handle_);
            }
        }

        if (freed_once) {
            drivers::command_list list = cmd_builder.retrieve_command_list();
            driver_->submit_command_list(list);
        }
    }

    bool egl_controller::make_current(kernel::uid thread_id, const egl_context_handle handle) {
        auto *context_ptr = contexts_.get(handle);
        egl_context *context_to_set = (context_ptr ? (*context_ptr).get() : nullptr);

        auto ite = active_context_.find(thread_id);
        if ((ite != active_context_.end()) && ite->second && (ite->second != context_to_set)) {
            if (ite->second->draw_surface_ && ite->second->draw_surface_->dead_pending_) {
                ite->second->cmd_builder_.destroy_bitmap(ite->second->draw_surface_->handle_);

                for (auto &var: dsurfaces_) {
                    if (var.get() == ite->second->draw_surface_) {
                        // Free slot
                        var = nullptr;
                    }
                }
            }

            if ((ite->second->draw_surface_ != ite->second->read_surface_) && ite->second->read_surface_ && ite->second->read_surface_->dead_pending_) {
                ite->second->cmd_builder_.destroy_bitmap(ite->second->read_surface_->handle_);

                for (auto &var: dsurfaces_) {
                    if (var.get() == ite->second->read_surface_) {
                        // Free slot
                        var = nullptr;
                    }
                }
            }

            if (ite->second->draw_surface_) {
                ite->second->draw_surface_->bounded_context_ = nullptr;
            }

            if (ite->second->read_surface_) {
                ite->second->read_surface_->bounded_context_ = nullptr;
            }

            ite->second->draw_surface_ = nullptr;
            ite->second->read_surface_ = nullptr;

            if (ite->second->dead_pending_) {
                ite->second->free(driver_, ite->second->cmd_builder_);
            } else {    
                // Submit pending works...
                drivers::command_list list = ite->second->cmd_builder_.retrieve_command_list();
                driver_->submit_command_list(list);
            }

            if (ite->second->dead_pending_) {
                for (auto &var: contexts_) {
                    if (var.get() == ite->second) {
                        // Free slot
                        var = nullptr;
                    }
                }
            }
        }

        active_context_[thread_id] = context_to_set;

        if (context_to_set)
            context_to_set->associated_thread_uid_ = thread_id;

        return true;
    }

    void egl_controller::clear_current(kernel::uid thread_id) {
        auto ite = active_context_.find(thread_id);
        if (ite != active_context_.end()) {
            active_context_.erase(ite);
        }
    }

    egl_context_handle egl_controller::add_context(egl_context_instance &instance) {
        return static_cast<egl_context_handle>(contexts_.add(instance));
    }

    egl_context *egl_controller::get_context(const egl_context_handle handle) {
        auto *res = contexts_.get(handle);
        if (res == nullptr) {
            return nullptr;
        }

        return (*res).get();
    }

    std::uint32_t egl_controller::add_managed_surface(egl_surface_instance &dsurface) {
        return static_cast<std::uint32_t>(dsurfaces_.add(dsurface));
    }

    void egl_controller::destroy_managed_surface(const std::uint32_t handle) {
        egl_surface_instance *inst = dsurfaces_.get(handle);
        
        if (inst) {
            bool can_del_imm = true;

            for (auto ite = active_context_.begin(); ite != active_context_.end(); ) {
                if ((ite->second->draw_surface_ == inst->get()) || (ite->second->read_surface_ == inst->get())) {
                    inst->get()->dead_pending_ = true;
                    can_del_imm = false;

                    break;
                }
            }

            if (can_del_imm) {
                if ((*inst)->handle_) {
                    drivers::graphics_command_builder builder;
                    builder.destroy_bitmap((*inst)->handle_);

                    drivers::command_list retrieved = builder.retrieve_command_list();
                    driver_->submit_command_list(retrieved);
                }

                dsurfaces_.remove(static_cast<std::size_t>(handle));
            }
        }
    }

    void egl_controller::remove_managed_surface_from_management(const egl_surface *surface) {
        for (auto &var: dsurfaces_) {
            if (var.get() == surface) {
                var = nullptr;
                break;
            }
        }
    }

    egl_surface *egl_controller::get_managed_surface(const std::uint32_t managed_handle) {
        auto *res = dsurfaces_.get(managed_handle);
        if (res == nullptr) {
            return nullptr;
        }

        return (*res).get();
    }

    void egl_controller::remove_context(const egl_context_handle handle) {
        auto *context_ptr = contexts_.get(handle);
        if (!context_ptr) {
            return;
        }

        bool can_del_imm = true;

        for (auto ite = active_context_.begin(); ite != active_context_.end(); ite++) {
            if (ite->second == context_ptr->get()) {
                ite->second->dead_pending_ = true;
                can_del_imm = false;

                break;
            }
        }

        if (can_del_imm) {
            drivers::graphics_command_builder builder;
            context_ptr->get()->free(driver_, builder);

            drivers::command_list retrieved = builder.retrieve_command_list();
            driver_->submit_command_list(retrieved);

            contexts_.remove(static_cast<std::size_t>(handle));
        }
    }

    void egl_controller::push_error(kernel::uid thread_id, const std::uint32_t error) {
        error_map_[thread_id] = error;
    }

    std::uint32_t egl_controller::pop_error(kernel::uid thread_id) {
        auto ite = error_map_.find(thread_id);
        if (ite == error_map_.end()) {
            return GL_NO_ERROR;
        }

        std::uint32_t result = ite->second;
        error_map_.erase(ite);

        return result;
    }

    void egl_controller::push_error(egl_context *context, const std::uint32_t error) {
        if (!context) {
            return;
        }

        push_error(context->associated_thread_uid_, error);
    }

    std::uint32_t egl_controller::pop_error(egl_context *context) {
        if (!context) {
            return GL_NO_ERROR;
        }

        return pop_error(context->associated_thread_uid_);
    }
    
    void egl_controller::push_egl_error(kernel::uid thread_id, const std::uint32_t error) {
        egl_error_map_[thread_id] = error;
    }

    std::uint32_t egl_controller::pop_egl_error(kernel::uid thread_id) {
        auto ite = egl_error_map_.find(thread_id);
        if (ite == egl_error_map_.end()) {
            return GL_NO_ERROR;
        }

        std::uint32_t result = ite->second;
        egl_error_map_.erase(ite);

        return result;
    }
    
    void egl_controller::set_graphics_driver(drivers::graphics_driver *driver) {
        driver_ = driver;
        es1_shaderman_.set_graphics_driver(driver);
    }

    egl_context *egl_controller::current_context(kernel::uid thread_id) {
        auto ite = active_context_.find(thread_id);
        if (ite == active_context_.end()) {
            return nullptr;
        }

        return ite->second;
    }

    void egl_config::set_buffer_size(const std::uint8_t buffer_size) {
        config_ &= ~0b110;
        config_ |= ((((buffer_size >> 3) - 1) & 0b11) << 1);
    }

    std::uint8_t egl_config::buffer_size() const {
        return (((config_ >> 1) & 0b11) + 1) << 3;
    }

    void egl_config::set_surface_type(const surface_type type) {;
        config_ &= ~0b1;
        config_ |= (static_cast<std::uint32_t>(type) & 0b1);
    }

    egl_config::surface_type egl_config::get_surface_type() const {
        return static_cast<surface_type>(config_ & 0b1);
    }

    egl_config::target_context_version egl_config::get_target_context_version() {
        return static_cast<target_context_version>((config_ >> 3) & 0b1);
    }

    void egl_config::set_target_context_version(const target_context_version ver) {
        config_ |= (ver & 1) << 3;
    }
}