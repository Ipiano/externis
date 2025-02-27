/**
 * Copyright (C) 2022 Roy Jacobson
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
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <gcc-plugin.h>

#include "externis.h"

#include <stack>
#include <string>
#include <vector>

#include "c-family/c-pragma.h"
#include "cpplib.h"
#include <tree-pass.h>

namespace externis {
time_point_t COMPILATION_START;

namespace {
map_t<std::string, int64_t> preprocess_start;
map_t<std::string, int64_t> preprocess_end;
std::stack<std::string> preprocessing_stack;
const char *CIRCULAR_POISON_VALUE = "CIRCULAR_POISON_VALUE_95d6021c";

TimeStamp last_function_parsed_ts;

struct OptPassEvent {
  const opt_pass *pass;
  TimeSpan ts;
};
OptPassEvent last_pass;
std::vector<OptPassEvent> pass_events;

map_t<std::string, std::string> file_to_include_directory;
map_t<std::string, std::string> normalized_files_map;
set_t<std::string> normalized_files;
set_t<std::string> conflicted_files;

bool starts_with(const std::string &check, const std::string &match) {
  return check.size() >= match.size() &&
         std::equal(match.begin(), match.end(), check.begin());
}

void register_include_location(const char *file_name, const char *dir_name) {
  if (!file_to_include_directory.count(file_name)) {
    std::string file_std = file_name;
    file_to_include_directory[file_name] = dir_name;
    auto &folder_std = file_to_include_directory[file_std];
    if (starts_with(file_std, folder_std)) {
      // +1 for path separator.
      auto normalized_file = file_std.substr(folder_std.size() + 1);
      normalized_files_map[file_std] = normalized_file;
      if (normalized_files.count(normalized_file)) {
        conflicted_files.insert(normalized_file);
      } else {
        normalized_files.insert(normalized_file);
      }
    } else {
      fprintf(stderr, "Externis warning: Can't normalize paths %s and %s\n",
              file_name, dir_name);
    }
  }
}

const char *normalized_file_name(const char *file_name) {
  if (normalized_files_map.count(file_name) and
      !conflicted_files.count(normalized_files_map[file_name])) {
    return normalized_files_map[file_name].data();
  } else {
    return file_name;
  }
}

EventCategory pass_type(opt_pass_type type) {
  switch (type) {
  case opt_pass_type::GIMPLE_PASS:
    return GIMPLE_PASS;
  case opt_pass_type::RTL_PASS:
    return RTL_PASS;
  case opt_pass_type::SIMPLE_IPA_PASS:
    return SIMPLE_IPA_PASS;
  case opt_pass_type::IPA_PASS:
    return IPA_PASS;
  }
  return UNKNOWN;
}

struct ScopeEvent {
  std::string name;
  EventCategory type;
  TimeSpan ts;

  ScopeEvent() {}
  ScopeEvent(const std::string &_name, EventCategory _type, TimeSpan _ts)
      : name(_name), type(_type), ts(_ts) {}
};
std::vector<ScopeEvent> scope_events;

struct FunctionEvent {
  std::string name;
  const char *file_name;
  TimeSpan ts;

  FunctionEvent() {}
  FunctionEvent(const std::string &_name, const char *_file_name, TimeSpan _ts)
      : name(_name), file_name(_file_name), ts(_ts) {}
};

std::vector<FunctionEvent> function_events;

} // namespace

void finish_preprocessing_stage() {
  while (!preprocessing_stack.empty()) {
    end_preprocess_file();
    last_function_parsed_ts = ns_from_start();
  }
}

void start_preprocess_file(const char *file_name, cpp_reader *pfile) {
  auto now = ns_from_start();
  if (!file_name || !strcmp(file_name, "<command-line>")) {
    return;
  }
  if (preprocess_start.count(file_name) && !preprocess_end.count(file_name)) {
    // This is an edge case - this means that file_name is somewhere down the
    // stack and we have a circular include. Big fun!
    // Because we don't want to add the inner include, we replace file_name
    // with a poison value and set pfile to nullptr.
    file_name = CIRCULAR_POISON_VALUE;
    pfile = nullptr;
  }

  if (!preprocess_start.count(file_name)) {
    preprocess_start[file_name] = now;
  }

  preprocessing_stack.push(file_name);
  // This finds out which folder the file was included from.
  if (pfile) {
    auto cpp_buffer = cpp_get_buffer(pfile);
    auto cpp_file = cpp_get_file(cpp_buffer);
    auto dir = cpp_get_dir(cpp_file);
    auto real_dir_name = realpath(dir->name, nullptr);
    auto real_file_name = realpath(file_name, nullptr);
    if (real_dir_name && real_file_name) {
      register_include_location(real_file_name, real_dir_name);
    } else {
      if (strcmp(dir->name, "")) {
        fprintf(stderr, "Externis error! Couldn't call realpath(\"%s\")\n",
                dir->name);
      }
    }
    if (real_dir_name) {
      free(real_dir_name);
    }
    if (real_file_name) {
      free(real_file_name);
    }
  }
}

void end_preprocess_file() {
  auto now = ns_from_start();
  if (!preprocess_end.count(preprocessing_stack.top())) {
    preprocess_end[preprocessing_stack.top()] = now;
  }
  preprocessing_stack.pop();
  last_function_parsed_ts = now + 3;
}

void write_preprocessing_events() {
  finish_preprocessing_stage(); // Should've already happened, but in any case.
  for (const auto &preprocess_item : preprocess_start) {
    const auto &file = preprocess_item.first;
    const auto &start = preprocess_item.second;
    if (file == CIRCULAR_POISON_VALUE) {
      continue;
    }
    int64_t end = preprocess_end.at(file);
    add_event(
        TraceEvent{normalized_file_name(file.data()),
                   EventCategory::PREPROCESS,
                   {start, end},
                   std::make_pair(false, map_t<std::string, std::string>())});
  }
}

void start_opt_pass(const opt_pass *pass) {
  auto now = ns_from_start();
  last_pass.ts.end = now;
  if (last_pass.pass) {
    pass_events.emplace_back(last_pass);
  }
  last_pass.pass = pass;
  last_pass.ts.start = now + 1;
}

void write_opt_pass_events() {
  for (const auto &event : pass_events) {
    map_t<std::string, std::string> args;
    args["static_pass_number"] = std::to_string(event.pass->static_pass_number);
    add_event(TraceEvent{event.pass->name, pass_type(event.pass->type),
                         event.ts, std::make_pair(true, std::move(args))});
  }
}

void end_parse_function(FinishedFunction info) {
  static bool did_last_function_have_scope = false;
  TimeStamp now = ns_from_start();

  // Because of UI bugs we can't have different events starting and ending
  // at the same time - so we adjust some of the events by a few nanoseconds.

  TimeSpan ts{last_function_parsed_ts + 3, now};
  last_function_parsed_ts = now;
  function_events.emplace_back(info.name, info.file_name, ts);

  if (info.scope_name) {
    if (!scope_events.empty() && did_last_function_have_scope &&
        scope_events.back().name == info.scope_name) {
      scope_events.back().ts.end = ts.end + 1;
    } else {
      scope_events.emplace_back(info.scope_name, info.scope_type,
                                TimeSpan{ts.start - 1, ts.end + 1});
    }
    did_last_function_have_scope = true;
  } else {
    did_last_function_have_scope = false;
  }
}

void write_all_scopes() {
  for (const auto &item : scope_events) {
    const auto &name = item.name;
    const auto &type = item.type;
    const auto &ts = item.ts;
    add_event(
        TraceEvent{name.data(), type, ts,
                   std::make_pair(false, map_t<std::string, std::string>())});
  }
}

void write_all_functions() {
  for (const auto &item : function_events) {
    const auto &name = item.name;
    const auto &file_name = item.file_name;
    const auto &ts = item.ts;
    map_t<std::string, std::string> args;
    args["file"] = normalized_file_name(file_name);
    add_event(TraceEvent{name.data(), EventCategory::FUNCTION, ts,
                         std::make_pair(true, std::move(args))});
  }
}
} // namespace externis
