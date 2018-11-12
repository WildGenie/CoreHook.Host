
#include "path_utils.h"

namespace coreload {

    pal::string_t get_filename(const pal::string_t& path) {

        if (path.empty()) {
            return path;
        }
        auto name_pos = path.find_last_of(DIR_SEPARATOR);
        if (name_pos == pal::string_t::npos) {
            return path;
        }
        return path.substr(name_pos + 1);
    }

    void append_path(pal::string_t* path1, const pal::char_t* path2) {
        if (pal::is_path_rooted(path2)) {
            path1->assign(path2);
        }
        else {
            if (!path1->empty() && path1->back() != DIR_SEPARATOR) {
                path1->push_back(DIR_SEPARATOR);
            }
            path1->append(path2);
        }
    }

    pal::string_t get_replaced_char(const pal::string_t& path, pal::char_t match, pal::char_t repl) {
        int pos = path.find(match);
        if (pos == pal::string_t::npos) {
            return path;
        }

        pal::string_t out = path;
        do {
            out[pos] = repl;
        } while ((pos = out.find(match, pos)) != pal::string_t::npos);

        return out;
    }
    pal::string_t get_directory(const pal::string_t& path) {
        pal::string_t ret = path;
        while (!ret.empty() && ret.back() == DIR_SEPARATOR) {
            ret.pop_back();
        }

        // Find the last dir separator
        auto path_sep = ret.find_last_of(DIR_SEPARATOR);
        if (path_sep == pal::string_t::npos) {
            return ret + DIR_SEPARATOR;
        }

        int pos = (int)path_sep;
        while (pos >= 0 && ret[pos] == DIR_SEPARATOR) {
            pos--;
        }

        return ret.substr(0, pos + 1) + DIR_SEPARATOR;
    }

    void remove_trailing_dir_seperator(pal::string_t* dir) {
        if (dir->back() == DIR_SEPARATOR) {
            dir->pop_back();
        }
    }

    void replace_char(pal::string_t* path, pal::char_t match, pal::char_t repl) {
        int pos = 0;
        while ((pos = path->find(match, pos)) != pal::string_t::npos) {
            (*path)[pos] = repl;
        }
    }

    size_t index_of_non_numeric(const pal::string_t& str, unsigned i) {
        return str.find_first_not_of(_X("0123456789"), i);
    }

    bool try_stou(const pal::string_t& str, unsigned* num) {
        if (str.empty()) {
            return false;
        }
        if (index_of_non_numeric(str, 0) != pal::string_t::npos) {
            return false;
        }
        *num = (unsigned)std::stoul(str);
        return true;
    }

    bool ends_with(const pal::string_t& value, const pal::string_t& suffix, bool match_case) {
        auto cmp = match_case ? pal::strcmp : pal::strcasecmp;
        return (value.size() >= suffix.size()) &&
            cmp(value.c_str() + value.size() - suffix.size(), suffix.c_str()) == 0;
    }

    pal::string_t strip_executable_ext(const pal::string_t& filename) {
        pal::string_t exe_suffix = pal::exe_suffix();
        if (exe_suffix.empty()) {
            return filename;
        }

        if (ends_with(filename, exe_suffix, false)) {
            // We need to strip off the old extension
            pal::string_t result(filename);
            result.erase(result.size() - exe_suffix.size());
            return result;
        }

        return filename;
    }
    pal::string_t strip_file_ext(const pal::string_t& path) {
        if (path.empty()) {
            return path;
        }
        size_t sep_pos = path.rfind(_X("/\\"));
        size_t dot_pos = path.rfind(_X('.'));
        if (sep_pos != pal::string_t::npos && sep_pos > dot_pos) {
            return path;
        }
        return path.substr(0, dot_pos);
    }
}