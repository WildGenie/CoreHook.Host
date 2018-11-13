#include "fx_definition.h"

namespace coreload {

    fx_definition_t::fx_definition_t() {}

    fx_definition_t::fx_definition_t(
        const pal::string_t& name,
        const pal::string_t& dir,
        const pal::string_t& requested_version,
        const pal::string_t& found_version) 
        : m_name(name)
        , m_dir(dir)
        , m_requested_version(requested_version)
        , m_found_version(found_version) {

    }

    void fx_definition_t::parse_runtime_config(
        const pal::string_t& path,
        const pal::string_t& dev_path,
        const runtime_config_t* higher_layer_config,
        const runtime_config_t* app_config
    )
    {
        m_runtime_config.parse(path, dev_path, higher_layer_config, app_config);
    }

    void fx_definition_t::parse_deps()
    {
        m_deps.parse(false, m_deps_file);
    }

    void fx_definition_t::parse_deps(const deps_json_t::rid_fallback_graph_t& graph)
    {
        m_deps.parse(true, m_deps_file, graph);
    }

}