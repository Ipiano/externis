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

#include "externis.h"

#include "c-family/c-pragma.h"
#include "cpplib.h"
#include <cassert>
#include <cp/cp-tree.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <options.h>
#include <string_view>
#include <tree-check.h>
#include <tree-pass.h>
#include <tree.h>

int plugin_is_GPL_compatible = 1;

namespace externis {

void cb_finish_parse_function(void *gcc_data, void *user_data) {
  tree decl = (tree)gcc_data;
  auto expanded_location = expand_location(decl->decl_minimal.locus);
  auto decl_name = decl_as_string(decl, 0);
  auto parent_decl = DECL_CONTEXT(decl);
  const char *scope_name = nullptr;
  externis::EventCategory scope_type = externis::EventCategory::UNKNOWN;
  if (parent_decl) {
    if (TREE_CODE(parent_decl) != TRANSLATION_UNIT_DECL) {
      scope_name = decl_as_string(parent_decl, 0);
      switch (TREE_CODE(parent_decl)) {
      case NAMESPACE_DECL:
        scope_type = externis::EventCategory::NAMESPACE;
        break;
      case RECORD_TYPE:
      case UNION_TYPE:
        scope_type = externis::EventCategory::STRUCT;
        break;
      default:
        fprintf(stderr, "Unkown tree code %d\n", TREE_CODE(parent_decl));
        break;
      }
    }
  }
  end_parse_function(FinishedFunction{
      gcc_data, decl_name, expanded_location.file, scope_name, scope_type});
}

void cb_plugin_finish(void *gcc_data, void *user_data) { write_all_events(); }

void (*old_file_change_cb)(cpp_reader *, const line_map_ordinary *);
void cb_file_change(cpp_reader *pfile, const line_map_ordinary *new_map) {
  if (new_map) {
    const char *file_name = ORDINARY_MAP_FILE_NAME(new_map);
    if (file_name) {
      switch (new_map->reason) {
      case LC_ENTER:
        start_preprocess_file(file_name, pfile);
        break;
      case LC_LEAVE:
        end_preprocess_file();
        break;
      default:
        break;
      }
    }
  }
  (*old_file_change_cb)(pfile, new_map);
}

void cb_start_compilation(void *gcc_data, void *user_data) {
  start_preprocess_file(main_input_filename, nullptr);
  cpp_callbacks *cpp_cbs = cpp_get_callbacks(parse_in);
  old_file_change_cb = cpp_cbs->file_change;
  cpp_cbs->file_change = cb_file_change;
}

void cb_pass_execution(void *gcc_data, void *user_data) {
  auto pass = (opt_pass *)gcc_data;
  start_opt_pass(pass);
}

void cb_finish_decl(void *gcc_data, void *user_data) {
  finish_preprocessing_stage();
}

} // namespace externis

static const char *PLUGIN_NAME = "externis";

bool setup_output(int argc, plugin_argument *argv) {
  const std::string_view file_flag_name = "trace";
  const std::string_view dir_flag_name = "trace-dir";

  const static std::filesystem::path default_filename = "trace_XXXXXX.json";

  std::optional<std::filesystem::path> target_file;
  std::optional<std::filesystem::path> target_dir;

  // TODO: Maybe make the default filename related to the source filename.
  // TODO: Validate we only compile one TU at a time.

  for (int i = 0; i < argc; ++i) {
    std::string_view arg_key = argv[i].key;

    if (arg_key == file_flag_name) {
      target_file = argv[i].value;
    } else if (arg_key == dir_flag_name) {
      target_dir = argv[i].value;
    } else {
      std::cerr << "Externis Error! Unknown argument '" << arg_key
                << "' - known arguments are [" << file_flag_name << ", "
                << dir_flag_name << "]\n";
      return false;
    }
  }

  if (target_dir && !target_dir->is_absolute()) {
    std::cerr << "Externis Error! " << dir_flag_name
              << " must be absolute; to output relative to the input "
                 "source, use "
              << file_flag_name << "\n";
  }

  if (target_file && target_dir) {
    std::cerr << "Externis Error! " << dir_flag_name
              << " may not be specified if" << file_flag_name
              << " is absolute\n";
    return false;
  }

  if (target_dir) {
    target_file = *target_dir / default_filename;
  }

  assert(target_file);

  FILE *trace_file = nullptr;

  std::string filename = *target_file;

  const auto replace_idx = filename.rfind("XXXXXX");
  if (replace_idx != std::string::npos) {
    int fd = mkstemps(filename.data(), filename.size() - (replace_idx + 6));

    if (fd == -1) {
      std::cerr << "Externis Error! Failed to determine temp directory from "
                << filename << "\n";
      perror("\tmkstemps error: ");
      return false;
    }
    trace_file = fdopen(fd, "w");
  } else {
    trace_file = fopen(target_file->c_str(), "w");
  }

  if (trace_file) {
    externis::set_output_file(trace_file);
    return true;
  } else {
    return false;
  }
}

int plugin_init(struct plugin_name_args *plugin_info,
                struct plugin_gcc_version *ver) {

  static struct plugin_info externis_info = {
      .version = "0.1", .help = "Generate time traces of the compilation."};
  externis::COMPILATION_START = externis::clock_t::now();
  if (!setup_output(plugin_info->argc, plugin_info->argv)) {
    return -1;
  }

  register_callback(PLUGIN_NAME, PLUGIN_FINISH_PARSE_FUNCTION,
                    &externis::cb_finish_parse_function, nullptr);
  register_callback(PLUGIN_NAME, PLUGIN_FINISH, &externis::cb_plugin_finish,
                    nullptr);
  register_callback(PLUGIN_NAME, PLUGIN_PASS_EXECUTION,
                    &externis::cb_pass_execution, nullptr);
  register_callback(PLUGIN_NAME, PLUGIN_START_UNIT,
                    &externis::cb_start_compilation, nullptr);
  register_callback(PLUGIN_NAME, PLUGIN_FINISH_DECL, &externis::cb_finish_decl,
                    nullptr);
  register_callback(PLUGIN_NAME, PLUGIN_INFO, nullptr, &externis_info);
  return 0;
}
