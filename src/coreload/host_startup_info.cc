#include "host_startup_info.h"
#include "pal.h"
#include "status_code.h"
#include "logging.h"
#include "utils.h"

namespace coreload {

    host_startup_info_t::host_startup_info_t(
        const pal::char_t* host_path_value,
        const pal::char_t* dotnet_root_value,
        const pal::char_t* app_path_value)
        : host_path(host_path_value)
        , dotnet_root(dotnet_root_value)
        , app_path(app_path_value) {}

    // Determine if string is a valid path, and if so then fix up by using realpath()
    bool get_path_from_argv(pal::string_t *path) {
        // Assume all paths will have at least one separator. We want to detect path vs. file before calling realpath
        // because realpath will expand a filename into a full path containing the current directory which may be
        // the wrong location when filename ends up being found in %PATH% and not the current directory.
        if (path->find(DIR_SEPARATOR) != pal::string_t::npos)
        {
            return pal::realpath(path);
        }

        return false;
    }

    int host_startup_info_t::parse(
        int argc,
        const pal::char_t* argv[]) {
        // Get host_path
        get_host_path(argc, argv, &host_path);

        // Get dotnet_root
        dotnet_root.assign(get_directory(host_path));

        // Get app_path
        app_path.assign(dotnet_root);
        pal::string_t app_name = get_filename(strip_executable_ext(host_path));
        append_path(&app_path, app_name.c_str());
        app_path.append(_X(".dll"));

        logging::logger::instance().info(_X("Host path: [%s]"), host_path.c_str());
        logging::logger::instance().info(_X("Dotnet path: [%s]"), dotnet_root.c_str());
        logging::logger::instance().info(_X("App path: [%s]"), app_path.c_str());
        return 0;
    }

    const bool host_startup_info_t::is_valid() const {
        return (
            !host_path.empty() &&
            !dotnet_root.empty() &&
            !app_path.empty());
    }

    const pal::string_t host_startup_info_t::get_app_name() const {
        return get_filename(strip_file_ext(app_path));
    }

    /*static*/
    int host_startup_info_t::get_host_path(int argc, const pal::char_t* argv[], pal::string_t* host_path) {
        // Attempt to get host_path from argv[0] as to allow for hosts located elsewhere
        if (argc >= 1) {
            host_path->assign(argv[0]);
            if (!host_path->empty()) {
                logging::logger::instance().info(_X("Attempting to use argv[0] as path [%s]"), host_path->c_str());
                if (!get_path_from_argv(host_path)) {
                    logging::logger::instance().warning(_X("Failed to resolve argv[0] as path [%s]. Using location of current executable instead."), host_path->c_str());
                    host_path->clear();
                }
            }
        }

        // If argv[0] did not work, get the executable name
        if (host_path->empty() && (!pal::get_own_executable_path(host_path) || !pal::realpath(host_path))) {
            logging::logger::instance().error(_X("Failed to resolve full path of the current executable [%s]"), host_path->c_str());
            return StatusCode::LibHostCurExeFindFailure;
        }

        return 0;
    }

}