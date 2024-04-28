/*
 * minicargo - MRustC-specific clone of `cargo`
 * - By John Hodge (Mutabah)
 *
 * build.cpp
 * - Logic related to invoking the compiler
 */
#ifdef _MSC_VER
# define _CRT_SECURE_NO_WARNINGS    // Allows use of getenv (this program doesn't set env vars)
#endif

#include "manifest.h"
#include "cfg.hpp"
#include "build.h"
#include "debug.h"
#include "stringlist.h"
#include "jobs.hpp"
#include "file_timestamp.h"
#include "os.hpp"
#include <fstream>
#include <cassert>

#include <unordered_map>
#include <algorithm>    // sort/find_if

#ifdef _WIN32
# define EXESUF ".exe"
# define DLLSUF ".dll"
#else
# define EXESUF ""
# define DLLSUF ".so"
#endif
#include <target_detect.h>  // tools/common/target_detect.h
#define HOST_TARGET DEFAULT_TARGET_NAME

struct RunState
{
    BuildOptions&   m_opts;
    const helpers::path& m_compiler_path;
    bool m_is_cross_compiling;

    RunState(BuildOptions& opts, bool is_cross_compiling)
        : m_opts(opts)
        , m_compiler_path(os_support::get_mrustc_path())
        , m_is_cross_compiling(is_cross_compiling)
    {}

    bool is_rustc() const {
        return m_compiler_path.basename() == "rustc" || m_compiler_path.basename() == "rustc.exe";
    }

    std::string get_key(const PackageManifest& p, bool build, bool is_host) const {
        auto rv = ::format(p.name(), " v", p.version());
        if(p.has_library() && p.get_library().m_is_proc_macro ) {
            is_host = true;
        }
        if(build) {
            rv += " (build)";
        }
        else if(is_host && m_is_cross_compiling) {
            rv += " (host)";
        }
        else {
        }
        return rv;
    }

    bool outfile_needs_rebuild(const helpers::path& outfile) const;

    /// Get the crate suffix (stuff added to the crate name to form the filename)
    ::std::string get_crate_suffix(const PackageManifest& manifest) const;
    /// Get the base of all build script names (relative to output dir)
    ::std::string get_build_script_out(const PackageManifest& manifest) const;
    ::std::string get_build_script_exe(const PackageManifest& manifest) const {
        return get_output_dir(true) / get_build_script_out(manifest) + "_run" EXESUF;
    }
    /// Get the output file for a crate (e.g. libfoo.rlib, or foo.exe)
    ::helpers::path get_crate_path(const PackageManifest& manifest, const PackageTarget& target, bool is_for_host, const char** crate_type, ::std::string* out_crate_suffix) const;
    
    // If `is_for_host` and cross compiling, use a different directory
    // - TODO: Include the target arch in the output dir too?
    ::helpers::path get_output_dir(bool is_for_host) const {
        if(is_for_host && (m_opts.target_name != nullptr && !m_opts.emit_mmir))
            return m_opts.output_dir / "host";
        else
            return m_opts.output_dir;
    }
};
class Job_Build: public Job
{
protected:
    const RunState&  parent;
    const PackageManifest&  m_manifest;

    ::std::string   m_name;
public:
    ::std::vector<std::string>  m_dependencies;

protected:
    Job_Build(const RunState& parent, const PackageManifest& manifest, ::std::string name)
        : parent(parent)
        , m_manifest(manifest)
        , m_name(::std::move(name))
    {
    }

    void push_args_common(StringList& args, const helpers::path& outfile, bool is_for_host) const;

public:
    const std::string& name() const override {
        return m_name;
    }
    const std::vector<std::string>& dependencies() const override {
        return m_dependencies;
    }
    bool is_runnable() const override {
        return true;
    }
    bool complete(bool was_success) override;
    virtual helpers::path get_outfile() const = 0;
};
class Job_BuildTarget: public Job_Build
{
    const PackageTarget&    m_target;
    const bool  m_is_for_host;

public:
    Job_BuildTarget(RunState& parent, const PackageManifest& manifest, const PackageTarget& target, bool is_host)
        : Job_Build(parent, manifest, parent.get_key(manifest, false, is_host))
        , m_target(target)
        , m_is_for_host(is_host)
    {
    }
    helpers::path   m_build_script;

    RunnableJob start() override;
    helpers::path get_outfile() const override;
};
class Job_BuildScript: public Job_Build
{
public:
    Job_BuildScript(const RunState& parent, const PackageManifest& manifest)
        : Job_Build(parent, manifest, parent.get_key(manifest, true, false))
    {
    }

    RunnableJob start() override;
    helpers::path get_outfile() const override;
};
class Job_RunScript: public Job
{
    const RunState&  parent;
    const PackageManifest&  m_manifest;
    const ::std::string m_name;
    // Populated on `start`
    helpers::path   m_script_exe_abs;
public:
    Job_RunScript(const RunState& parent, const PackageManifest& manifest)
        : parent(parent)
        , m_manifest(manifest)
        , m_name(parent.get_key(manifest, false, false)+" (script run)")
    {
    }
    ::std::vector<std::string>  m_dependencies;

    const char* verb() const override {
        return "RUNNING";
    }
    const std::string& name() const override {
        return m_name;
    }
    const std::vector<std::string>& dependencies() const override {
        return m_dependencies;
    }
    bool is_runnable() const override {
        return true;
    }
    
    RunnableJob start() override;
    bool complete(bool was_success) override;

    helpers::path get_script_exe() const;
    helpers::path get_outfile() const;
};

BuildList::BuildList(const PackageManifest& manifest, const BuildOptions& opts):
    m_root_manifest(manifest)
{
    struct ListBuilder {
        struct Ent {
            const PackageManifest* package;
            bool    native;
            unsigned level;
            };
        ::std::vector<Ent>  m_list;

        void add_package(const PackageManifest& p, unsigned level, bool include_build, bool is_native)
        {
            TRACE_FUNCTION_F(p.name() << (is_native ? " host" : ""));
            // If this is a proc macro, force `is_native`
            if(p.has_library() && p.get_library().m_is_proc_macro ) {
                is_native = true;
            }
            // If the package is already loaded
            for(auto& ent : m_list)
            {
                if(ent.package == &p && ent.native == is_native && ent.level >= level)
                {
                    // NOTE: Only skip if this package will be built before we needed (i.e. the level is greater)
                    return ;
                }
                // Keep searching (might already have a higher entry)
            }
            m_list.push_back({ &p, is_native, level });
            add_dependencies(p, level, include_build, is_native);
        }
        void add_dependencies(const PackageManifest& p, unsigned level, bool include_build, bool is_native)
        {
            p.iter_main_dependencies([&](const PackageRef& dep) {
                if( !dep.is_disabled() )
                {
                    DEBUG(p.name() << ": Dependency " << dep.name());
                    add_package(dep.get_package(), level+1, include_build, is_native);
                }
            });

            if( p.build_script() != "" && include_build )
            {
                p.iter_build_dependencies([&](const PackageRef& dep) {
                    if( !dep.is_disabled() )
                    {
                        DEBUG(p.name() << ": Build Dependency " << dep.name());
                        add_package(dep.get_package(), level+1, true, true);
                    }
                });
            }
        }
        void sort_list()
        {
            ::std::sort(m_list.begin(), m_list.end(), [](const auto& a, const auto& b){ return a.level > b.level; });

            // Needed to deduplicate after sorting (`add_package` doesn't fully dedup)
            for(auto it = m_list.begin(); it != m_list.end(); )
            {
                auto it2 = ::std::find_if(m_list.begin(), it, [&](const auto& x){ return x.package == it->package && x.native == it->native; });
                if( it2 != it )
                {
                    DEBUG((it - m_list.begin()) << ": Duplicate " << it->package->name() << " " << (it->native ? "host":"") << " - Already at pos " << (it2 - m_list.begin()));
                    it = m_list.erase(it);
                }
                else
                {
                    DEBUG((it - m_list.begin()) << ": Keep " << it->package->name() << " " << (it->native ? "host":"") << ", level = " << it->level);
                    ++it;
                }
            }
        }
    };
    bool cross_compiling = (opts.target_name != nullptr);
    ListBuilder b;
    b.add_dependencies(manifest, 0, !opts.build_script_overrides.is_valid(), !cross_compiling);
    if( manifest.has_library() )
    {
        b.m_list.push_back({ &manifest, !cross_compiling, 0 });
    }
    if( opts.mode != BuildOptions::Mode::Normal )   // Tests or examples
    {
        manifest.iter_dev_dependencies([&](const PackageRef& dep) {
            if( !dep.is_disabled() )
            {
                DEBUG(manifest.name() << ": Dependency " << dep.name());
                b.add_package(dep.get_package(), 1, !opts.build_script_overrides.is_valid(), !cross_compiling);
            }
        });
    }

    b.sort_list();
    
    // Move the contents of the above list to this class's list
    m_list.reserve(b.m_list.size());
    for(const auto& e : b.m_list)
    {
        m_list.push_back({ e.package, e.native, {} });
    }
}
bool BuildList::build(BuildOptions opts, unsigned num_jobs, bool dry_run)
{
    bool cross_compiling = (opts.target_name != nullptr && !opts.emit_mmir);

    RunState    run_state { opts, cross_compiling };
    JobList runner;

    struct ConvertState {
        JobList& joblist;
        ::std::unordered_map<std::string,bool>  items_built;
        ::std::unordered_map<std::string,Timestamp>   items_notbuilt;
        ConvertState(JobList& joblist): joblist(joblist) {}

        bool handle_dep(std::vector<std::string>& job_deps, const Timestamp& output_ts, const std::string& k) const {
            if( items_built.find(k) != items_built.end() ) {
                // Add the dependency
                job_deps.push_back(k);
                return true;
            }
            else {
                auto it = items_notbuilt.find(k);
                if(it == items_notbuilt.end()) {
                    ::std::cerr << "ASSERTION Failed: items_notbuilt.find('"<<k<<"') != items_notbuilt.end" <<std::endl;
                    abort();
                }
                // This crate's output is older than the depencency, force a rebuild
                return output_ts < it->second;
            }
        }
        void add_job(::std::unique_ptr<Job> job, Timestamp ts, bool is_needed) {
            if(is_needed) {
                DEBUG("Dirty " << job->name());
                // Add as built
                items_built.insert(std::make_pair(job->name(), false));
                joblist.add_job(std::move(job));
            }
            else {
                DEBUG("Clean " << job->name());
                // Add as not-built
                items_notbuilt.insert(std::make_pair(job->name(), ts));
            }
        }

        // Generate jobs for a build script
        // Returns the job name (or an empty string, if no job generated)
        // Populates the build script path
        std::string handle_build_script(RunState& run_state, const PackageManifest& p, const helpers::path& build_script_overrides, helpers::path& build_script, bool is_host)
        {
            if( p.build_script() != "" )
            {
                if( build_script_overrides.is_valid() )
                {
                    auto override_file = build_script_overrides / "build_" + p.name().c_str() + ".txt";
                    // TODO: Should this test if it exists? or just assume and let it error?

                    build_script = override_file;
                    const_cast<PackageManifest&>(p).load_build_script( build_script.str() );
                    return "";
                }
                else
                {
                    auto job_bs_build = ::std::make_unique<Job_BuildScript>(run_state, p);
                    
                    auto script_ts = Timestamp::for_file(job_bs_build->get_outfile());
                    bool bs_is_dirty = run_state.outfile_needs_rebuild(job_bs_build->get_outfile());
                    p.iter_build_dependencies([&](const PackageRef& dep) {
                        if( !dep.is_disabled() )
                        {
                            auto k = run_state.get_key(dep.get_package(), false, /*is_host=*/true);
                            DEBUG("BS Dep: " << k);
                            bs_is_dirty |= this->handle_dep(job_bs_build->m_dependencies, script_ts, k);
                        }
                    });
                    auto name_bs_build = job_bs_build->name();
                    this->add_job(std::move(job_bs_build), script_ts, bs_is_dirty);

                    auto job_bs_run = ::std::make_unique<Job_RunScript>(run_state, p);
                    build_script = job_bs_run->get_outfile();

                    if( run_state.m_opts.emit_mmir )
                    {
                        // HACK: Search for `-mmir/` in the output, remove it, and if that exists copy it to here
                        // - This grabs the last non-mmir execution of the script
                        auto tmp_out = build_script.str();
                        auto mmir_pos = tmp_out.rfind("-mmir/");
                        if( mmir_pos != std::string::npos )
                        {
                            auto src = tmp_out.substr(0, mmir_pos) + tmp_out.substr(mmir_pos+5);
                            std::ifstream   ifs(src);
                            if( ifs.good() )
                            {
                                std::cout << "HACK: Copying " << src << " to " << tmp_out << std::endl;
                                {
                                    ::std::ofstream ofs(tmp_out);
                                    ofs << ifs.rdbuf();
                                }
                                const_cast<PackageManifest&>(p).load_build_script( build_script.str() );
                                return name_bs_build;
                            }
                        }
                        // Fall back to trying (and failing) to run the script
                    }

                    auto output_ts = Timestamp::for_file(build_script);
                    this->handle_dep(job_bs_run->m_dependencies, output_ts, name_bs_build);
                    p.iter_main_dependencies([&](const PackageRef& dep) {
                        if( !dep.is_disabled() )
                        {
                            auto k = run_state.get_key(dep.get_package(), false, is_host);
                            DEBUG("BS Main Dep: " << k);
                            bs_is_dirty |= this->handle_dep(job_bs_run->m_dependencies, output_ts, k);
                        }
                    });
                    bool bs_needs_run = bs_is_dirty || output_ts < script_ts;
                    auto rv = bs_needs_run ? job_bs_run->name() : ::std::string();
                    this->add_job(std::move(job_bs_run), output_ts, bs_needs_run);
                    // If the script is not being run, then it still needs to be loaded
                    if(!bs_needs_run)
                    {
                        const_cast<PackageManifest&>(p).load_build_script( build_script.str() );
                    }
                    return rv;
                }
            }
            else
            {
                return "";
            }
        }
    } convert_state(runner);
    
    for(const auto& e : m_list)
    {
        const auto& p = *e.package;

        auto job = ::std::make_unique<Job_BuildTarget>(run_state, p, p.get_library(), e.is_host);
        DEBUG("> Considering " << job->name());

        auto output_ts = Timestamp::for_file(job->get_outfile());
        bool is_dirty = run_state.outfile_needs_rebuild(job->get_outfile());
        // Handle build script
        auto bs_job_name = convert_state.handle_build_script(run_state, p, opts.build_script_overrides, job->m_build_script, e.is_host);
        if( bs_job_name != "" ) {
            job->m_dependencies.push_back(bs_job_name);
            is_dirty = true;
        }
        // Check dependencies
        p.iter_main_dependencies([&](const PackageRef& dep) {
            if( !dep.is_disabled() )
            {
                auto k = run_state.get_key(dep.get_package(), false, e.is_host);
                DEBUG("Dep " << k);
                is_dirty |= convert_state.handle_dep(job->m_dependencies, output_ts, k);
            }
        });
        convert_state.add_job(std::move(job), output_ts, is_dirty);
    }

    std::string bs_job_name;
    helpers::path   build_script;
    if( !m_root_manifest.has_library() )
    {
        bs_job_name = convert_state.handle_build_script(run_state, m_root_manifest, opts.build_script_overrides, build_script, !cross_compiling);
    }

    auto push_root_target = [&](const PackageTarget& target) {
        const bool is_host = !cross_compiling;
        auto job = ::std::make_unique<Job_BuildTarget>(run_state, m_root_manifest, target, is_host);
        auto output_ts = Timestamp::for_file(job->get_outfile());
        bool is_dirty = run_state.outfile_needs_rebuild(job->get_outfile());
        if( m_root_manifest.has_library() ) {
            auto k = run_state.get_key(m_root_manifest, false, is_host);
            is_dirty |= convert_state.handle_dep(job->m_dependencies, output_ts, k);
        }
        else {
            m_root_manifest.iter_main_dependencies([&](const PackageRef& dep) {
                if( !dep.is_disabled() )
                {
                    auto k = run_state.get_key(dep.get_package(), false, is_host);
                    is_dirty |= convert_state.handle_dep(job->m_dependencies, output_ts, k);
                }
            });
        }
        convert_state.add_job(std::move(job), output_ts, is_dirty);
    };

    // Now that all libraries are done, build the binaries (if present)
    switch(opts.mode)
    {
    case BuildOptions::Mode::Normal:
        this->m_root_manifest.foreach_binaries([&](const PackageTarget& bin_target)->bool {
            push_root_target(bin_target);
            return true;
            });
        break;
    case BuildOptions::Mode::Test:
        this->m_root_manifest.foreach_ty(PackageTarget::Type::Test, [&](const PackageTarget& test_target)->bool {
            push_root_target(test_target);
            return true;
            });
        break;
    //case BuildOptions::Mode::Examples:
    }
    
    return runner.run_all(num_jobs, dry_run);
}

namespace {
    ::std::map< ::std::string, ::std::vector<helpers::path> > load_depfile(const helpers::path& depfile_path)
    {
        ::std::map< ::std::string, ::std::vector<helpers::path> >   rv;
        ::std::ifstream ifp(depfile_path);
        if( ifp.good() )
        {
            // Load space-separated (backslash-escaped) paths
            struct Lexer {
                ::std::ifstream ifp;
                unsigned m_line;
                char    m_c;

                Lexer(::std::ifstream ifp)
                    :ifp(::std::move(ifp))
                    ,m_line(1)
                    ,m_c(0)
                {
                    nextc();
                }

                bool nextc() {
                    int v = ifp.get();
                    if( v == EOF ) {
                        m_c = '\0';
                        return false;
                    }
                    else {
                        m_c = (char)v;
                        return true;
                    }
                }
                ::std::string get_token() {
                    auto t = get_token_int();
                    //DEBUG("get_token '" << t << "'");
                    return t;
                }
                ::std::string get_token_int() {
                    if( ifp.eof() && m_c == '\0')
                        return "";
                    while( m_c == ' ' )
                    {
                        if( !nextc() )
                            return "";
                    }
                    if( m_c == '\n' ) {
                        nextc();
                        return "\n";
                    }
                    if( m_c == '\t' ) {
                        nextc();
                        return "\t";
                    }
                    ::std::string   rv;
                    do {
                        if( m_c == '\\' )
                        {
                            nextc();
                            if( m_c == ' ' ) {
                                rv += m_c;
                            }
                            else if( m_c == ':' ) {
                                rv += m_c;
                            }
                            // HACK: Only spaces are escaped this way?
                            else {
                                rv += '\\';
                                rv += m_c;
                            }
                        }
                        else
                        {
                            rv += m_c;
                        }
                    } while( nextc() && m_c != ' ' && m_c != ':' && m_c != '\n' );
                    return rv;
                }
            }   lexer(::std::move(ifp));

            // Look for <string> ":" [<string>]* "\n"
            do {
                auto t = lexer.get_token();
                if( t == "" )
                    break;
                if( t == "\n" )
                    continue ;

                auto v = rv.insert(::std::make_pair(t, ::std::vector<helpers::path>()));
                auto& list = v.first->second;
                auto target = t;
                t = lexer.get_token();
                if( t != ":" ) {
                    ::std::cerr << depfile_path << ":" << lexer.m_line << ": Malformed depfile, expected ':' but got '" << t << "'" << std::endl;
                    throw ::std::runtime_error("Malformed depfile: No `:` after filename");
                }

                do {
                    t = lexer.get_token();
                    if( t == "\n" || t == "" )
                        break ;
                    list.push_back(t);
                } while(1);
            } while(1);
        }
        return rv;
    }

    std::string escape_dashes(const std::string& s) {
        std::string rv;
        for(char c : s)
            rv += (c == '-' ? '_' : c);
        return rv;
    }
    // Common environment variables for compiling (build scripts and libraries)
    void push_env_common(StringListKV& env, const PackageManifest& manifest)
    {
        env.push_back("CARGO_MANIFEST_DIR", manifest.directory().to_absolute());
        env.push_back("CARGO_PKG_NAME", manifest.name());
        env.push_back("CARGO_PKG_VERSION", ::format(manifest.version()));
        env.push_back("CARGO_PKG_VERSION_MAJOR", ::format(manifest.version().major));
        env.push_back("CARGO_PKG_VERSION_MINOR", ::format(manifest.version().minor));
        env.push_back("CARGO_PKG_VERSION_PATCH", ::format(manifest.version().patch));
        // - Downstream environment variables
        manifest.iter_main_dependencies([&](const PackageRef& dep) {
            if( ! dep.is_disabled() )
            {
                const auto& m = dep.get_package();
                for(const auto& p : m.build_script_output().downstream_env)
                {
                    env.push_back(p.first.c_str(), p.second.c_str());
                }
            }
        });
    }
    void push_args_edition(StringList& args, Edition edition)
    {
        switch(edition)
        {
        case Edition::Unspec:
            break;
        case Edition::Rust2015:
            args.push_back("--edition");
            args.push_back("2015");
            break;
        case Edition::Rust2018:
            args.push_back("--edition");
            args.push_back("2018");
            break;
        }
    }
}

bool RunState::outfile_needs_rebuild(const helpers::path& outfile) const
{
    auto ts_result = Timestamp::for_file(outfile);
    if( ts_result == Timestamp::infinite_past() ) {
        // Rebuild (missing)
        DEBUG("Building " << outfile << " - Missing");
        return true;
    }
    else if( !getenv("MINICARGO_IGNTOOLS") && ts_result < Timestamp::for_file(m_compiler_path) ) {
        // Rebuild (older than mrustc/minicargo)
        DEBUG("Building " << outfile << " - Older than mrustc (" << ts_result << " < " << Timestamp::for_file(m_compiler_path) << ")");
        return true;
    }
    else {
        // Check dependencies. (from depfile)
        auto depfile_ents = load_depfile(outfile + ".d");
        auto it = depfile_ents.find(outfile);
        bool has_new_file = false;
        if( it != depfile_ents.end() )
        {
            for(const auto& f : it->second)
            {
                auto dep_ts = Timestamp::for_file(f);
                if( ts_result < dep_ts )
                {
                    has_new_file = true;
                    DEBUG("Rebuilding " << outfile << ", older than " << f << " (" << ts_result << " < " << dep_ts << ")");
                    break;
                }
            }
        }

        if( !has_new_file )
        {
            // Don't rebuild (no need to)
            DEBUG("Not building " << outfile << " - not out of date");
            return false;
        }
        return true;
    }
}

::std::string RunState::get_build_script_out(const PackageManifest& manifest) const
{
    return std::string("build_") + manifest.name().c_str() + (manifest.version() == PackageVersion() ? "" : get_crate_suffix(manifest).c_str());
}
::helpers::path RunState::get_crate_path(const PackageManifest& manifest, const PackageTarget& target, bool is_for_host, const char** crate_type, ::std::string* out_crate_suffix) const
{
    auto outfile = this->get_output_dir(is_for_host);

    auto crate_suffix = get_crate_suffix(manifest);

    if(out_crate_suffix)
        *out_crate_suffix = crate_suffix;

    if( manifest.version() == PackageVersion() ) 
    {
        crate_suffix = "";
    }

    switch(target.m_type)
    {
    case PackageTarget::Type::Lib:
        switch( target.m_crate_types.size() > 0
                ? target.m_crate_types.front()
                : (target.m_is_proc_macro
                    ? PackageTarget::CrateType::proc_macro
                    : PackageTarget::CrateType::rlib
                  )
              )
        {
        case PackageTarget::CrateType::proc_macro:
            if(crate_type)  *crate_type = "proc-macro";
            outfile /= ::format("lib", target.m_name, crate_suffix, "-plugin" EXESUF);
            break;
        case PackageTarget::CrateType::dylib:
            if( getenv("MINICARGO_DYLIB") )
            {
                // TODO: Enable this once mrustc can set rpath or absolute paths
                if(crate_type)  *crate_type = "dylib";
                outfile /= ::format("lib", target.m_name, crate_suffix, DLLSUF);
                break;
            }
        case PackageTarget::CrateType::rlib:
            if(crate_type)  *crate_type = "rlib";
            outfile /= ::format("lib", target.m_name, crate_suffix, ".rlib");
            break;
        default:
            throw "";
        }
        break;
    case PackageTarget::Type::Bin:
        if(crate_type)
            *crate_type = "bin";
        outfile /= ::format(target.m_name, EXESUF);
        break;
    case PackageTarget::Type::Test:
        if(crate_type)
            *crate_type = "bin";
        outfile /= ::format(target.m_name, EXESUF);
        break;
    default:
        throw ::std::runtime_error("Unknown target type being built");
    }
    return outfile;
}


bool Job_Build::complete(bool was_success)
{
    if(!was_success) {
        // On failure, remove the output (to force a rebuild next time)
        remove(get_outfile().str().c_str());
    }
    return true;
}
void Job_Build::push_args_common(StringList& args, const helpers::path& outfile, bool is_for_host) const
{
    args.push_back("-o"); args.push_back(outfile);
    if( !parent.is_rustc() ) {
        args.push_back("-C"); args.push_back(format("emit-depfile=",outfile,".d"));
    }
    else {
        args.push_back("--emit"); args.push_back("link,dep-info");
    }
    if( parent.m_opts.enable_debug ) {
        args.push_back("-g");
    }
    if( true ) {
        args.push_back("--cfg"); args.push_back("debug_assertions");
    }
    if( true /*parent.m_opts.enable_optimise*/ ) {
        args.push_back("-O");
    }
    if( parent.m_opts.emit_mmir ) {
        args.push_back("-C"); args.push_back("codegen-type=monomir");
    }

    for(const auto& d : parent.m_opts.lib_search_dirs)
    {
        args.push_back("-L");
        if( is_for_host && parent.m_opts.target_name && !parent.m_opts.emit_mmir ) {
            // HACK! Look for `-TARGETNAME` in the output path, and erase it
            // - This turns `output-1.54-TARGETNAME` into `output-1.54`, pulling the non-cross-compiled libraries instead of the XC'd ones
            auto tp = std::string("-")+parent.m_opts.target_name;
            auto needle_pos = d.str().rfind(tp);
            if( needle_pos != std::string::npos )
            {
                auto src = d.str().substr(0, needle_pos) + (d.str().c_str() + (needle_pos+tp.size()));
                args.push_back(src);
            }
            else {
                args.push_back(d.str().c_str());
            }
        }
        else {
            args.push_back(d.str().c_str());
        }
    }
    args.push_back("-L"); args.push_back(parent.get_output_dir(is_for_host).str());
    // HACK
    if( !is_for_host ) {
        if( parent.m_opts.target_name && !parent.m_opts.emit_mmir ) {
            args.push_back("-L"); args.push_back(parent.get_output_dir(true).str());
        }
    }

    for(const auto& feat : m_manifest.active_features()) {
        args.push_back("--cfg"); args.push_back(::format("feature=\"", feat, "\""));
    }
}

//
//
//
helpers::path Job_BuildTarget::get_outfile() const
{
    return parent.get_crate_path(m_manifest, m_target, m_is_for_host, nullptr, nullptr);
}
RunnableJob Job_BuildTarget::start()
{
    const char* crate_type;
    ::std::string   crate_suffix;
    auto outfile = parent.get_crate_path(m_manifest, m_target, m_is_for_host,  &crate_type, &crate_suffix);
    auto depfile = outfile + ".d";

    StringList  args;
    args.push_back(::helpers::path(m_manifest.manifest_path()).parent() / ::helpers::path(m_target.m_path));
    push_args_common(args, outfile, m_is_for_host);
    args.push_back("--crate-name"); args.push_back(m_target.m_name.c_str());
    args.push_back("--crate-type"); args.push_back(crate_type);
    if( !crate_suffix.empty() ) {
        if( !parent.is_rustc() ) {
            args.push_back("--crate-tag"); args.push_back(format(crate_suffix.c_str() + 1));
        }
        else {
            args.push_back("-C"); args.push_back(format("metadata=",crate_suffix.c_str() + 1));
            if( outfile.str().find(crate_suffix) != std::string::npos ) {
                args.push_back("-C"); args.push_back(format("extra-filename=",crate_suffix.c_str()));
            }
        }
    }

    if( parent.m_opts.target_name )
    {
        if( m_is_for_host ) {
            //args.push_back("--target"); args.push_back(HOST_TARGET);
        }
        else {
            args.push_back("--target"); args.push_back(parent.m_opts.target_name);
            args.push_back("-C"); args.push_back(format("emit-build-command=",outfile,".sh"));
        }
    }

    for(const auto& dir : m_manifest.build_script_output().rustc_link_search) {
        args.push_back("-L"); args.push_back(dir.second.c_str());
    }
    for(const auto& lib : m_manifest.build_script_output().rustc_link_lib) {
        if(!strcmp(lib.first, "framework")) {
            args.push_back("-l"); args.push_back(format("framework=",lib.second.c_str()));
        }
        else {
            args.push_back("-l"); args.push_back(lib.second.c_str());
        }
    }
    for(const auto& cfg : m_manifest.build_script_output().rustc_cfg) {
        args.push_back("--cfg"); args.push_back(cfg.c_str());
    }
    for(const auto& flag : m_manifest.build_script_output().rustc_flags) {
        args.push_back(flag.c_str());
    }

    // If not building the package's library, but the package has a library
    if( m_target.m_type != PackageTarget::Type::Lib && m_manifest.has_library() )
    {
        // Add a --extern for it
        auto path = parent.get_crate_path(m_manifest, m_manifest.get_library(), m_is_for_host, nullptr, nullptr);
        args.push_back("--extern");
        args.push_back(::format(m_manifest.get_library().m_name, "=", path));
    }
    push_args_edition(args, m_target.m_edition);
    if( m_target.m_type == PackageTarget::Type::Test )
    {
        args.push_back("--test");
    }
    m_manifest.iter_main_dependencies([&](const PackageRef& dep) {
        if( ! dep.is_disabled() )
        {
            const auto& m = dep.get_package();
            auto path = parent.get_crate_path(m, m.get_library(), m_is_for_host || (m.has_library() && m.get_library().m_is_proc_macro), nullptr, nullptr);
            args.push_back("--extern");
            if( dep.key() != m.name() ) {
                args.push_back(::format(escape_dashes(dep.key()), "=", path));
            }
            else {
                args.push_back(::format(m.get_library().m_name, "=", path));
            }
        }
    });
    if( m_target.m_type == PackageTarget::Type::Test )
    {
        m_manifest.iter_dev_dependencies([&](const PackageRef& dep) {
            if( ! dep.is_disabled() )
            {
                const auto& m = dep.get_package();
                auto path = parent.get_crate_path(m, m.get_library(), m_is_for_host, nullptr, nullptr);
                args.push_back("--extern");
                args.push_back(::format(escape_dashes(dep.key()), "=", path));
            }
        });
    }

    // Environment variables (rustc_env)
    StringListKV    env;
    auto out_dir = parent.get_output_dir(m_is_for_host).to_absolute() / parent.get_build_script_out(m_manifest);
    env.push_back("OUT_DIR", out_dir.str());
    for(const auto& e : m_manifest.build_script_output().rustc_env) {
        env.push_back(e.first.c_str(), e.second.c_str());
    }
    push_env_common(env, m_manifest);

    return RunnableJob(parent.m_compiler_path.str().c_str(), std::move(args), std::move(env), outfile + "_dbg.txt");
}

//
//
//
helpers::path Job_BuildScript::get_outfile() const
{
    return parent.get_build_script_exe(m_manifest);
}
RunnableJob Job_BuildScript::start()
{
    auto outfile = get_outfile();

    StringList  args;
    args.push_back( ::helpers::path(m_manifest.manifest_path()).parent() / ::helpers::path(m_manifest.build_script()) );
    push_args_common(args, outfile, /*is_for_host=*/true);
    args.push_back("--crate-name"); args.push_back("build");
    args.push_back("--crate-type"); args.push_back("bin");
    push_args_edition(args, m_manifest.edition());

    m_manifest.iter_build_dependencies([&](const PackageRef& dep) {
        if( ! dep.is_disabled() )
        {
            const auto& m = dep.get_package();
            auto path = parent.get_crate_path(m, m.get_library(), true, nullptr, nullptr);   // Dependencies for build scripts are always for the host (because it is)
            args.push_back("--extern"); args.push_back(::format(m.get_library().m_name, "=", path));
        }
    });
    // - Build scripts are built for the host (not the target)
    //args.push_back("--target"); args.push_back(HOST_TARGET);

    StringListKV    env;
    push_env_common(env, m_manifest);

    // TODO: If there's any dependencies marked as `links = foo` then grab `DEP_FOO_<varname>` from its metadata
    // (build script output)

    return RunnableJob(parent.m_compiler_path.str().c_str(), std::move(args), std::move(env), outfile + "_dbg.txt");
}

//
//
//
helpers::path Job_RunScript::get_outfile() const
{
    return parent.get_output_dir(true) / parent.get_build_script_out(m_manifest) + ".txt";
}
helpers::path Job_RunScript::get_script_exe() const
{
    return parent.get_build_script_exe(m_manifest);
}
RunnableJob Job_RunScript::start()
{
    auto out_dir = parent.get_output_dir(true) / parent.get_build_script_out(m_manifest);
    auto out_file = get_outfile();
    auto script_exe = get_script_exe();

    auto script_exe_abs = script_exe.to_absolute();

    // - Run the script and put output in the right dir
    os_support::mkdir(out_dir);
    // Environment variables (key-value list)
    StringListKV    env;
    //env.push_back("CARGO_MANIFEST_LINKS", manifest.m_links);
    for(const auto& feat : m_manifest.active_features())
    {
        ::std::string   fn = "CARGO_FEATURE_";
        for(char c : feat)
            fn += c == '-' ? '_' : toupper(c);
        env.push_back(fn, "1");
    }
    //env.push_back("CARGO_CFG_RELEASE", "");
    env.push_back("OUT_DIR", out_dir.to_absolute());

    push_env_common(env, m_manifest);

    env.push_back("TARGET", parent.m_opts.target_name ? parent.m_opts.target_name : HOST_TARGET);
    env.push_back("HOST", HOST_TARGET);
    env.push_back("NUM_JOBS", "1");
    env.push_back("OPT_LEVEL", "2");
    env.push_back("DEBUG", "0");
    env.push_back("PROFILE", "release");
    // - Needed for `regex`'s build script, make mrustc pretend to be rustc
    env.push_back("RUSTC", parent.m_compiler_path);
    if( !parent.m_opts.lib_search_dirs.empty() ) {
        env.push_back("MRUSTC_LIBDIR", ::helpers::path(parent.m_opts.lib_search_dirs.front()).to_absolute().str());
    }

    // NOTE: All cfg(foo_bar) become CARGO_CFG_FOO_BAR
    Cfg_ToEnvironment(env);

    m_script_exe_abs = ::std::move(script_exe_abs);
    if( parent.m_opts.emit_mmir )
    {
        StringList  args;
        args.push_back(m_script_exe_abs.str() + ".mir");
        args.push_back("--logfile");
        args.push_back(out_file.to_absolute().str() + "-smiri.log");
        return RunnableJob("/home/tpg/Projects/mrustc/bin/standalone_miri", std::move(args), std::move(env), out_file.to_absolute(), m_manifest.directory());
    }
    else
    {
        return RunnableJob(m_script_exe_abs.str().c_str(), {}, std::move(env), out_file.to_absolute(), m_manifest.directory());
    }
}
bool Job_RunScript::complete(bool was_success)
{
    auto out_file = this->get_outfile();
    if(was_success)
    {
        // TODO: Parse the script here? Or just keep the parsing in the downstream build
        const_cast<PackageManifest&>(m_manifest).load_build_script( out_file.str() );
        return true;
    }
    else
    {
        auto failed_filename = out_file+"_failed.txt";
        remove(failed_filename.str().c_str());
        rename(out_file.str().c_str(), failed_filename.str().c_str());

        if(false)
        {
            ::std::ifstream ifs(failed_filename);
            char    linebuf[1024];
            while( ifs.good() && !ifs.eof() )
            {
                ifs.getline(linebuf, sizeof(linebuf)-1);
                if( strncmp(linebuf, "cargo:", 6) == 0 ) {
                    continue;
                }
                ::std::cerr << "> " << linebuf << ::std::endl;
            }
            ::std::cerr << "Calling " << this->get_script_exe() << " failed" << ::std::endl;
        }
        else
        {
            ::std::cerr << "Calling " << this->get_script_exe() << " failed (see " << failed_filename << " for stdout)" << ::std::endl;
        }

        return true;
    }
}

::std::string RunState::get_crate_suffix(const PackageManifest& manifest) const
{
    ::std::string   crate_suffix;
    // HACK: If there's no version, don't emit a version tag?
    //if( manifest.version() != PackageVersion() ) 
    {
        crate_suffix = ::format("-", manifest.version());
        for(auto& v : crate_suffix)
            if(v == '.')
                v = '_';
        // TODO: Hash/encode the following:
        // - Manifest path
        // - Feature set
        // For now, just emit a bitset of enabled features
        if( manifest.active_features().size() > 0 )
        {
            uint64_t mask = 0;
            size_t i = 0;
            for(auto it = manifest.all_features().begin(); it != manifest.all_features().end(); ++it, i ++)
            {
                if( std::count(manifest.active_features().begin(), manifest.active_features().end(), it->first) )
                {
                    mask |= (1ull << i);
                }
                if(i == 63)
                    break;
            }
            std::stringstream   ss;
            ss << crate_suffix;
            ss << "_H";
            ss << std::hex;
            ss << mask;

            crate_suffix = std::move(ss.str());
        }
    }
    return crate_suffix;
}
bool Builder::build_library(const PackageManifest& manifest, bool is_for_host, size_t index) const
{
    if( manifest.build_script() != "" )
    {
        // Locate a build script override file
        if(this->m_opts.build_script_overrides.is_valid())
        {
            //auto override_file = this->m_opts.build_script_overrides / get_build_script_out(manifest) + ".txt";
            auto override_file = this->m_opts.build_script_overrides / "build_" + manifest.name().c_str() + ".txt";
            // TODO: Should this test if it exists? or just assume and let it error?

            // > Note, override file can specify a list of commands to run.
            const_cast<PackageManifest&>(manifest).load_build_script( override_file.str() );
        }
        else
        {
            // - Build+Run
            auto script_file = this->build_and_run_script(manifest, is_for_host);
            if( !script_file.is_valid() )
            {
                return false;
            }
            // - Load
            const_cast<PackageManifest&>(manifest).load_build_script( script_file.str() );
        }
    }

    return this->build_target(manifest, manifest.get_library(), is_for_host, index);
}
bool Builder::spawn_process_mrustc(const StringList& args, StringListKV env, const ::helpers::path& logfile) const
{
    //env.push_back("MRUSTC_DEBUG", "");
    auto rv = spawn_process(m_compiler_path.str().c_str(), args, env, logfile);
    if(getenv("MINICARGO_RUN_ONCE") || getenv("MINICARGO_RUNONCE"))
    {
        if(rv) {
            std::cerr << "- Only running compiler once" << std::endl;
        }
        exit(1);
    }
    return rv;
}

const helpers::path& get_mrustc_path()
{
    static helpers::path    s_compiler_path;
    if( !s_compiler_path.is_valid() )
    {
        if( const char* override_path = getenv("MRUSTC_PATH") ) {
            s_compiler_path = override_path;
            return s_compiler_path;
        }
        // TODO: Clean this stuff up
#ifdef _WIN32
        char buf[1024];
        size_t s = GetModuleFileName(NULL, buf, sizeof(buf)-1);
        buf[s] = 0;

        ::helpers::path minicargo_path { buf };
        minicargo_path.pop_component();
        // MSVC, minicargo and mrustc are in the same dir
        s_compiler_path = minicargo_path / "mrustc.exe";
#else
        char buf[PATH_MAX];
# if defined(__linux__) || defined(__CYGWIN__)
        ssize_t s = readlink("/proc/self/exe", buf, sizeof(buf)-1);
        if(s >= 0)
        {
            buf[s] = 0;
        }
        else
# elif defined(__APPLE__)
        uint32_t  s = sizeof(buf);
        if( _NSGetExecutablePath(buf, &s) == 0 )
        {
            // Buffer populated
        }
        else
            // TODO: Buffer too small
# elif defined(__FreeBSD__) || defined(__DragonFly__) || (defined(__NetBSD__) && defined(KERN_PROC_PATHNAME)) // NetBSD 8.0+
        int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
        size_t s = sizeof(buf);
        if ( sysctl(mib, 4, buf, &s, NULL, 0) == 0 )
        {
            // Buffer populated
        }
        else
# else
        #   warning "Can't runtime determine path to minicargo"
# endif
        {
            // On any error, just hard-code as if running from root dir
            strcpy(buf, "tools/bin/minicargo");
        }

        ::helpers::path minicargo_path { buf };
        minicargo_path.pop_component();
        s_compiler_path = (minicargo_path / "mrustc").normalise();
#endif
    }
    return s_compiler_path;
}

#ifdef _WIN32
// Escapes an argument for CommandLineToArgv on Windows
void argv_quote_windows(const std::string& arg, std::stringstream& cmdline)
{
    if (arg.empty()) return;
    // Add a space to start a new argument.
    cmdline << " ";

    // Don't quote unless we need to
    if (arg.find_first_of(" \t\n\v\"") == arg.npos)
    {
        cmdline << arg;
        return;
    }
    else
    {
        cmdline << '"';
        for (auto ch = arg.begin(); ; ++ch) {
            size_t backslash_count = 0;

            // Count backslashes
            while (ch != arg.end() && *ch == L'\\') {
                ++ch;
                ++backslash_count;
            }

            if (ch == arg.end()) {
                // Escape backslashes, but let the terminating
                // double quotation mark we add below be interpreted
                // as a metacharacter.
                for (int i = 0; i < backslash_count * 2; i++) cmdline << '\\';
                break;
            }
            else if (*ch == L'"')
            {
                // Escape backslashes and the following double quotation mark.
                for (int i = 0; i < backslash_count * 2 + 1; i++) cmdline << '\\';
                cmdline << *ch;
            }
            else
            {
                for (int i = 0; i < backslash_count; i++) cmdline << '\\';
                cmdline << *ch;
            }
        }
        cmdline << '"';
    }
}
#endif

bool spawn_process(const char* exe_name, const StringList& args, const StringListKV& env, const ::helpers::path& logfile, const ::helpers::path& working_directory/*={}*/)
{
    if( getenv("MINICARGO_DUMPENV") )
    {
        ::std::stringstream environ_str;
        for(auto kv : env)
        {
            environ_str << kv.first << "=" << kv.second << ' ';
        }
        std::cout << environ_str.str() << std::endl;
    }

    // TODO: Support running with a debugger
    // - Determine if this is not the automatic initial run for `cfg`
    // - Put the exe name in the first arg
    // - Update the executable name

#ifdef _WIN32
    ::std::stringstream cmdline;
    cmdline << exe_name;
    for (const auto& arg : args.get_vec())
        argv_quote_windows(arg, cmdline);
    auto cmdline_str = cmdline.str();
    if(true)
    {
#ifndef DISABLE_MULTITHREAD
        ::std::lock_guard<::std::mutex> lh { s_cout_mutex };
#endif
        ::std::cout << "> " << cmdline_str << ::std::endl;
    }
    else
    {
        DEBUG("Calling " << cmdline_str);
    }

#if 0
    // TODO: Determine required minimal environment, to avoid importing the entire caller environment
    ::std::stringstream environ_str;
    environ_str << "TEMP=" << getenv("TEMP") << '\0';
    environ_str << "TMP=" << getenv("TMP") << '\0';
    for(auto kv : env)
    {
        environ_str << kv.first << "=" << kv.second << '\0';
    }
    environ_str << '\0';
#else
    for(auto kv : env)
    {
        DEBUG("putenv " << kv.first << "=" << kv.second);
        _putenv_s(kv.first, kv.second);
    }
#endif

    {
        auto logfile_dir = logfile.parent();
        if(logfile_dir.is_valid())
        {
            CreateDirectory(logfile_dir.str().c_str(), NULL);
        }
    }

    STARTUPINFO si = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = NULL;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    {
        SECURITY_ATTRIBUTES sa = { 0 };
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        si.hStdOutput = CreateFile( static_cast<::std::string>(logfile).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
        DWORD   tmp;
        WriteFile(si.hStdOutput, cmdline_str.data(), static_cast<DWORD>(cmdline_str.size()), &tmp, NULL);
        WriteFile(si.hStdOutput, "\n", 1, &tmp, NULL);
    }
    PROCESS_INFORMATION pi = { 0 };
    CreateProcessA(exe_name, (LPSTR)cmdline_str.c_str(), NULL, NULL, TRUE, 0, NULL, (working_directory != ::helpers::path() ? working_directory.str().c_str() : NULL), &si, &pi);
    CloseHandle(si.hStdOutput);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD status = 1;
    GetExitCodeProcess(pi.hProcess, &status);
    if (status != 0)
    {
#ifndef DISABLE_MULTITHREAD
        ::std::lock_guard<::std::mutex> lh { s_cout_mutex };
#endif
        set_console_colour(std::cerr, TerminalColour::Red);
        std::cerr << "Process `" << cmdline_str << "` exited with non-zero exit status " << status;
        set_console_colour(std::cerr, TerminalColour::Default);
        std::cerr << std::endl;
        return false;
    }

#else   // ^^ WIN32 / VV posix

    // Create logfile output directory
    mkdir(static_cast<::std::string>(logfile.parent()).c_str(), 0755);

    // Create handles such that the log file is on stdout
    ::std::string logfile_str = logfile;
    pid_t pid;
    posix_spawn_file_actions_t  fa;
    {
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, logfile_str.c_str(), O_CREAT|O_WRONLY|O_TRUNC, 0644);
    }

    // Generate `argv`
    auto argv = args.get_vec();
    argv.insert(argv.begin(), exe_name);

    if(true)
    {
        ::std::lock_guard<::std::mutex> lh { s_cout_mutex };
        ::std::cout << ">";
        for(const auto& p : argv)
            ::std::cout  << " " << p;
        ::std::cout << ::std::endl;
    }
    else
    {
        Debug_Print([&](auto& os){
            os << "Calling";
            for(const auto& p : argv)
                os << " " << p;
            });
    }
    DEBUG("Environment " << env);
    argv.push_back(nullptr);

    // Generate `envp`
    StringList  envp;
    extern char **environ;
    for(auto p = environ; *p; p++)
    {
        envp.push_back(*p);
    }
    for(auto kv : env)
    {
        envp.push_back(::format(kv.first, "=", kv.second));
    }
    //Debug_Print([&](auto& os){
    //    os << "ENVP=";
    //    for(const auto& p : envp.get_vec())
    //        os << "\n " << p;
    //    });
    envp.push_back(nullptr);

    {
        static ::std::mutex    s_chdir_mutex;
        ::std::lock_guard<::std::mutex> lh { s_chdir_mutex };
        auto fd_cwd = open(".", O_DIRECTORY);
        if( working_directory != ::helpers::path() ) {
            chdir(working_directory.str().c_str());
        }
        if( posix_spawn(&pid, exe_name, &fa, /*attr=*/nullptr, (char* const*)argv.data(), (char* const*)envp.get_vec().data()) != 0 )
        {
#ifndef DISABLE_MULTITHREAD
            ::std::lock_guard<::std::mutex> lh { s_cout_mutex };
#endif
            set_console_colour(std::cerr, TerminalColour::Red);
            ::std::cerr << "Unable to run process '" << exe_name << "' - " << strerror(errno);
            set_console_colour(std::cerr, TerminalColour::Default);
            ::std::cerr << ::std::endl;
            DEBUG("Unable to spawn executable");
            posix_spawn_file_actions_destroy(&fa);
            return false;
        }
        if( working_directory != ::helpers::path() ) {
            fchdir(fd_cwd);
        }
    }
    posix_spawn_file_actions_destroy(&fa);
    int status = -1;
    waitpid(pid, &status, 0);
    if( status != 0 )
    {
#ifndef DISABLE_MULTITHREAD
        ::std::lock_guard<::std::mutex> lh { s_cout_mutex };
#endif
        set_console_colour(std::cerr, TerminalColour::Red);
        if( WIFEXITED(status) )
            ::std::cerr << "Process exited with non-zero exit status " << WEXITSTATUS(status) << ::std::endl;
        else if( WIFSIGNALED(status) )
            ::std::cerr << "Process was terminated with signal " << WTERMSIG(status) << ::std::endl;
        else
            ::std::cerr << "Process terminated for unknown reason, status=" << status << ::std::endl;
        set_console_colour(std::cerr, TerminalColour::Default);
        ::std::cerr << "FAILING COMMAND: ";
        for(const auto& p : argv)
            if (p != nullptr)
                ::std::cerr  << " " << p;
        ::std::cerr << ::std::endl;
        //::std::cerr << "See " << logfile << " for the compiler output" << ::std::endl;
        return false;
    }
    else
    {
        DEBUG("Successful exit");
    }
#endif
    return true;
}

Timestamp Timestamp::for_file(const ::helpers::path& path)
{
#if _WIN32
    FILETIME    out;
    auto handle = CreateFile(path.str().c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if(handle == INVALID_HANDLE_VALUE) {
        //DEBUG("Can't find " << path);
        return Timestamp::infinite_past();
    }
    if( GetFileTime(handle, NULL, NULL, &out) == FALSE ) {
        //DEBUG("Can't GetFileTime on " << path);
        CloseHandle(handle);
        return Timestamp::infinite_past();
    }
    CloseHandle(handle);
    //DEBUG(Timestamp{out} << " " << path);
    return Timestamp { out };
#else
    struct stat  s;
    if( stat(path.str().c_str(), &s) == 0 )
    {
        return Timestamp { s.st_mtime };
    }
    else
    {
        return Timestamp::infinite_past();
    }
#endif
}

