/*
 * Copyright (c) 2016 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <linux/bpf.h>
#include <unistd.h>
#include <cstdio>
#include <exception>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

#include "bcc_exception.h"
#include "bcc_syms.h"
#include "bpf_module.h"
#include "libbpf.h"
#include "perf_reader.h"

#include "BPF.h"

namespace ebpf {

std::string uint_to_hex(uint64_t value) {
  std::stringstream ss;
  ss << std::hex << value;
  return ss.str();
}

std::string sanitize_str(std::string str, bool (*validator)(char),
                         char replacement = '_') {
  for (size_t i = 0; i < str.length(); i++)
    if (!validator(str[i]))
      str[i] = replacement;
  return str;
}

StatusTuple BPF::init(const std::string& bpf_program,
                      std::vector<std::string> cflags) {
  auto flags_len = cflags.size();
  const char* flags[flags_len];
  for (size_t i = 0; i < flags_len; i++)
    flags[i] = cflags[i].c_str();
  if (bpf_module_->load_string(bpf_program, flags, flags_len) != 0)
    return StatusTuple(-1, "Unable to initialize BPF program");
  return StatusTuple(0);
};

BPF::~BPF() {
  auto res = detach_all();
  if (res.code() != 0)
    std::cerr << "Failed to detach all probes on destruction: " << std::endl
              << res.msg() << std::endl;
}

StatusTuple BPF::detach_all() {
  bool has_error = false;
  std::string error_msg;

  for (auto it : kprobes_) {
    auto res = detach_kprobe_event(it.first, it.second);
    if (res.code() != 0) {
      error_msg += "Failed to detach kprobe event " + it.first + ": ";
      error_msg += res.msg() + "\n";
      has_error = true;
    }
  }

  for (auto it : uprobes_) {
    auto res = detach_uprobe_event(it.first, it.second);
    if (res.code() != 0) {
      error_msg += "Failed to detach uprobe event " + it.first + ": ";
      error_msg += res.msg() + "\n";
      has_error = true;
    }
  }

  for (auto it : tracepoints_) {
    auto res = detach_tracepoint_event(it.first, it.second);
    if (res.code() != 0) {
      error_msg += "Failed to detach Tracepoint " + it.first + ": ";
      error_msg += res.msg() + "\n";
      has_error = true;
    }
  }

  for (auto it : perf_buffers_) {
    auto res = it.second->close();
    if (res.code() != 0) {
      error_msg += "Failed to close perf buffer " + it.first + ": ";
      error_msg += res.msg() + "\n";
      has_error = true;
    }
    delete it.second;
  }

  if (has_error)
    return StatusTuple(-1, error_msg);
  else
    return StatusTuple(0);
}

StatusTuple BPF::attach_kprobe(const std::string& kernel_func,
                               const std::string& probe_func,
                               bpf_attach_type attach_type,
                               pid_t pid, int cpu, int group_fd,
                               perf_reader_cb cb, void* cb_cookie) {
  int probe_fd;
  TRY2(load_func(probe_func, BPF_PROG_TYPE_KPROBE, probe_fd));

  std::string probe_event = get_kprobe_event(kernel_func, attach_type);
  if (kprobes_.find(probe_event) != kprobes_.end()) {
    TRY2(unload_func(probe_func));
    return StatusTuple(-1, "kprobe %s already attached", probe_event.c_str());
  }

  std::string probe_event_desc = attach_type_prefix(attach_type);
  probe_event_desc += ":kprobes/" + probe_event + " " + kernel_func;

  void* res =
      bpf_attach_kprobe(probe_fd, probe_event.c_str(), probe_event_desc.c_str(),
                        pid, cpu, group_fd, cb, cb_cookie);

  if (!res) {
    TRY2(unload_func(probe_func));
    return StatusTuple(-1, "Unable to attach %skprobe for %s using %s",
                       attach_type_debug(attach_type).c_str(),
                       kernel_func.c_str(), probe_func.c_str());
  }

  open_probe_t p = {};
  p.reader_ptr = res;
  p.func = probe_func;
  kprobes_[probe_event] = std::move(p);
  return StatusTuple(0);
}

StatusTuple BPF::attach_uprobe(const std::string& binary_path,
                               const std::string& symbol,
                               const std::string& probe_func,
                               uint64_t symbol_addr,
                               bpf_attach_type attach_type,
                               pid_t pid, int cpu, int group_fd,
                               perf_reader_cb cb, void* cb_cookie) {
  int probe_fd;
  TRY2(load_func(probe_func, BPF_PROG_TYPE_KPROBE, probe_fd));

  bcc_symbol sym = bcc_symbol();
  TRY2(check_binary_symbol(binary_path, symbol, symbol_addr, &sym));

  std::string probe_event =
      get_uprobe_event(sym.module, sym.offset, attach_type);
  if (uprobes_.find(probe_event) != uprobes_.end()) {
    TRY2(unload_func(probe_func));
    return StatusTuple(-1, "uprobe %s already attached", probe_event.c_str());
  }

  std::string probe_event_desc = attach_type_prefix(attach_type);
  probe_event_desc += ":uprobes/" + probe_event + " ";
  probe_event_desc += binary_path + ":0x" + uint_to_hex(sym.offset);

  void* res =
      bpf_attach_uprobe(probe_fd, probe_event.c_str(), probe_event_desc.c_str(),
                        pid, cpu, group_fd, cb, cb_cookie);

  if (!res) {
    TRY2(unload_func(probe_func));
    return StatusTuple(
        -1,
        "Unable to attach %suprobe for binary %s symbol %s addr %lx using %s\n",
        attach_type_debug(attach_type).c_str(), binary_path.c_str(),
        symbol.c_str(), symbol_addr, probe_func.c_str());
  }

  open_probe_t p = {};
  p.reader_ptr = res;
  p.func = probe_func;
  uprobes_[probe_event] = std::move(p);
  return StatusTuple(0);
}

StatusTuple BPF::attach_tracepoint(const std::string& tracepoint,
                                   const std::string& probe_func,
                                   pid_t pid, int cpu, int group_fd,
                                   perf_reader_cb cb, void* cb_cookie) {
  int probe_fd;
  TRY2(load_func(probe_func, BPF_PROG_TYPE_TRACEPOINT, probe_fd));

  if (tracepoints_.find(tracepoint) != tracepoints_.end())
    return StatusTuple(-1, "Tracepoint %s already attached",
                       tracepoint.c_str());

  auto pos = tracepoint.find(":");
  if ((pos == std::string::npos) || (pos != tracepoint.rfind(":")))
    return StatusTuple(-1, "Unable to parse Tracepoint %s", tracepoint.c_str());
  std::string tp_category = tracepoint.substr(0, pos);
  std::string tp_name = tracepoint.substr(pos + 1);

  void* res =
      bpf_attach_tracepoint(probe_fd, tp_category.c_str(), tp_name.c_str(), pid,
                            cpu, group_fd, cb, cb_cookie);

  if (!res) {
    TRY2(unload_func(probe_func));
    return StatusTuple(-1, "Unable to attach Tracepoint %s using %s",
                       tracepoint.c_str(), probe_func.c_str());
  }

  open_probe_t p = {};
  p.reader_ptr = res;
  p.func = probe_func;
  tracepoints_[tracepoint] = std::move(p);
  return StatusTuple(0);
}

StatusTuple BPF::detach_kprobe(const std::string& kernel_func,
                               bpf_attach_type attach_type) {
  std::string event = get_kprobe_event(kernel_func, attach_type);

  auto it = kprobes_.find(event);
  if (it == kprobes_.end())
    return StatusTuple(-1, "No open %skprobe for %s",
                       attach_type_debug(attach_type).c_str(),
                       kernel_func.c_str());

  TRY2(detach_kprobe_event(it->first, it->second));
  kprobes_.erase(it);
  return StatusTuple(0);
}

StatusTuple BPF::detach_uprobe(const std::string& binary_path,
                               const std::string& symbol, uint64_t symbol_addr,
                               bpf_attach_type attach_type) {
  bcc_symbol sym = bcc_symbol();
  TRY2(check_binary_symbol(binary_path, symbol, symbol_addr, &sym));

  std::string event = get_uprobe_event(sym.module, sym.offset, attach_type);
  auto it = uprobes_.find(event);
  if (it == uprobes_.end())
    return StatusTuple(-1, "No open %suprobe for binary %s symbol %s addr %lx",
                       attach_type_debug(attach_type).c_str(),
                       binary_path.c_str(), symbol.c_str(), symbol_addr);

  TRY2(detach_uprobe_event(it->first, it->second));
  uprobes_.erase(it);
  return StatusTuple(0);
}

StatusTuple BPF::detach_tracepoint(const std::string& tracepoint) {
  auto it = tracepoints_.find(tracepoint);
  if (it == tracepoints_.end())
    return StatusTuple(-1, "No open Tracepoint %s", tracepoint.c_str());

  TRY2(detach_tracepoint_event(it->first, it->second));
  tracepoints_.erase(it);
  return StatusTuple(0);
}

StatusTuple BPF::open_perf_buffer(const std::string& name,
                                  perf_reader_raw_cb cb, void* cb_cookie) {
  if (perf_buffers_.find(name) == perf_buffers_.end())
    perf_buffers_[name] = new BPFPerfBuffer(bpf_module_.get(), name);
  auto table = perf_buffers_[name];
  TRY2(table->open(cb, cb_cookie));
  return StatusTuple(0);
}

StatusTuple BPF::close_perf_buffer(const std::string& name) {
  auto it = perf_buffers_.find(name);
  if (it == perf_buffers_.end())
    return StatusTuple(-1, "Perf buffer for %s not open", name.c_str());
  TRY2(it->second->close());
  return StatusTuple(0);
}

void BPF::poll_perf_buffer(const std::string& name, int timeout) {
  auto it = perf_buffers_.find(name);
  if (it == perf_buffers_.end())
    return;
  it->second->poll(timeout);
}

StatusTuple BPF::load_func(const std::string& func_name,
                           enum bpf_prog_type type, int& fd) {
  if (funcs_.find(func_name) != funcs_.end()) {
    fd = funcs_[func_name];
    return StatusTuple(0);
  }

  uint8_t* func_start = bpf_module_->function_start(func_name);
  if (!func_start)
    return StatusTuple(-1, "Can't find start of function %s",
                       func_name.c_str());
  size_t func_size = bpf_module_->function_size(func_name);

  fd = bpf_prog_load(type, reinterpret_cast<struct bpf_insn*>(func_start),
                     func_size, bpf_module_->license(),
                     bpf_module_->kern_version(), nullptr,
                     0  // BPFModule will handle error printing
                     );

  if (fd < 0)
    return StatusTuple(-1, "Failed to load %s: %d", func_name.c_str(), fd);
  funcs_[func_name] = fd;
  return StatusTuple(0);
}

StatusTuple BPF::unload_func(const std::string& func_name) {
  auto it = funcs_.find(func_name);
  if (it == funcs_.end())
    return StatusTuple(-1, "Probe function %s not loaded", func_name.c_str());

  int res = close(it->second);
  if (res != 0)
    return StatusTuple(-1, "Can't close FD for %s: %d", it->first.c_str(), res);

  funcs_.erase(it);
  return StatusTuple(0);
}

StatusTuple BPF::check_binary_symbol(const std::string& binary_path,
                                     const std::string& symbol,
                                     uint64_t symbol_addr, bcc_symbol* output) {
  int res = bcc_resolve_symname(binary_path.c_str(), symbol.c_str(),
                                symbol_addr, output);
  if (res < 0)
    return StatusTuple(
        -1, "Unable to find offset for binary %s symbol %s address %lx",
        binary_path.c_str(), symbol.c_str(), symbol_addr);
  return StatusTuple(0);
}

std::string BPF::get_kprobe_event(const std::string& kernel_func,
                                  bpf_attach_type type) {
  std::string res = attach_type_prefix(type) + "_";
  res += sanitize_str(kernel_func, &BPF::kprobe_event_validator);
  return res;
}

std::string BPF::get_uprobe_event(const std::string& binary_path,
                                  uint64_t offset, bpf_attach_type type) {
  std::string res = attach_type_prefix(type) + "_";
  res += sanitize_str(binary_path, &BPF::uprobe_path_validator);
  res += "_0x" + uint_to_hex(offset);
  return res;
}

StatusTuple BPF::detach_kprobe_event(const std::string& event,
                                     open_probe_t& attr) {
  if (attr.reader_ptr) {
    perf_reader_free(attr.reader_ptr);
    attr.reader_ptr = nullptr;
  }
  TRY2(unload_func(attr.func));
  std::string detach_event = "-:kprobes/" + event;
  if (bpf_detach_kprobe(detach_event.c_str()) < 0)
    return StatusTuple(-1, "Unable to detach kprobe %s", event.c_str());
  return StatusTuple(0);
}

StatusTuple BPF::detach_uprobe_event(const std::string& event,
                                     open_probe_t& attr) {
  if (attr.reader_ptr) {
    perf_reader_free(attr.reader_ptr);
    attr.reader_ptr = nullptr;
  }
  TRY2(unload_func(attr.func));
  std::string detach_event = "-:uprobes/" + event;
  if (bpf_detach_uprobe(detach_event.c_str()) < 0)
    return StatusTuple(-1, "Unable to detach uprobe %s", event.c_str());
  return StatusTuple(0);
}

StatusTuple BPF::detach_tracepoint_event(const std::string& tracepoint,
                                         open_probe_t& attr) {
  if (attr.reader_ptr) {
    perf_reader_free(attr.reader_ptr);
    attr.reader_ptr = nullptr;
  }
  TRY2(unload_func(attr.func));

  // TODO: bpf_detach_tracepoint currently does nothing.
  return StatusTuple(0);
}

}  // namespace ebpf
