#include "config.hpp"
#include "cmdline/cmdline.hpp"
#include "general/errors.hpp"
#include "general/tcp_util.hpp"
#include "spdlog/spdlog.h"
#include "toml++/toml.hpp"
#include "version.hpp"
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#ifdef __APPLE__
#include <sys/types.h>
#include <unistd.h>
#endif

using namespace std;

std::string get_default_datadir()
{
    const char* osBaseDir = nullptr;
#ifdef __linux__
    if ((osBaseDir = getenv("HOME")) == NULL) {
        osBaseDir = getpwuid(getuid())->pw_dir;
    }
    if (osBaseDir == nullptr)
        throw std::runtime_error("Cannot determine default data directory.");
    return std::string(osBaseDir) + "/.warthog/";
#elif _WIN32
    osBaseDir = getenv("LOCALAPPDATA");
    if (osBaseDir == nullptr)
        throw std::runtime_error("Cannot determine default data directory.");
    return std::string(osBaseDir) + "/Warthog/";
#elif __APPLE__
    if ((osBaseDir = getenv("HOME")) == NULL) {
        osBaseDir = getpwuid(getuid())->pw_dir;
    }
    if (osBaseDir == nullptr)
        throw std::runtime_error("Cannot determine default data directory.");
    return std::string(osBaseDir) + "/Library/Warthog/";
#else
    throw std::runtime_error("Cannot determine default data directory.");
#endif
}

Config::Config()
    : defaultDataDir(get_default_datadir())
{
}

namespace {
std::optional<SnapshotSigner> parse_leader_key(std::string privKey)
{
    try {
        SnapshotSigner ss { PrivKey(privKey) };
        spdlog::warn("This node signs chain snapshots with priority {}", ss.get_importance());
        return ss;
    } catch (Error e) {
        spdlog::warn("Cannot parse leader key, ignoring.");
    }
    return {};
}

}

int Config::init(int argc, char** argv)
{
    // default peer

    gengetopt_args_info ai;
    if (cmdline_parser(argc, argv, &ai) != 0) {
        return -1;
    }
    int res = process_gengetopt(ai);
    cmdline_parser_free(&ai);
    return res;
}
namespace {
void warning_config(const toml::key k)
{
    spdlog::warn("Ignoring configuration setting \""s + std::string(k) + "\" (line "s + std::to_string(k.source().begin.line) + ")");
}

template <typename T>
[[nodiscard]] auto fetch(toml::node& n)
{
    auto val = n.value<T>();
    if (val) {
        return val.value();
    }
    throw std::runtime_error("Cannot extract configuration value starting at line "s + std::to_string(n.source().begin.line) + ", colum "s + std::to_string(n.source().begin.column) + ".");
}

EndpointAddress fetch_endpointaddress(toml::node& n)
{
    auto p = EndpointAddress::parse(fetch<std::string>(n));
    if (p) {
        return p.value();
    }
    throw std::runtime_error("Cannot extract configuration value starting at line "s + std::to_string(n.source().begin.line) + ", colum "s + std::to_string(n.source().begin.column) + ".");
}
toml::array& array_ref(toml::node& n)
{
    if (n.is_array()) {
        return *n.as_array();
    }
    throw std::runtime_error("Expecting array at line "s + std::to_string(n.source().begin.line) + ".");
}
std::vector<EndpointAddress> parse_endpoints(std::string csv)
{
    std::vector<EndpointAddress> out;
    std::string::size_type pos = 0;
    while (true) {
        auto end = csv.find(",", pos);
        auto param = csv.substr(pos, end - pos);
        auto parsed = EndpointAddress::parse(param);
        if (!parsed) {
            throw std::runtime_error("Invalid parameter '"s + param + "'."s);
        }
        out.push_back(parsed.value());
        if (end == std::string::npos) {
            break;
        }
        pos = end + 1;
    }
    return out;
}
} // namespace

int Config::process_gengetopt(gengetopt_args_info& ai)
{
    bool dmp(ai.dump_config_given);
    if (!dmp)
        spdlog::info("Warthog Node v{}.{}.{} ", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);

    // Log
    if (ai.debug_given)
        spdlog::set_level(spdlog::level::debug);

    if (ai.peers_db_given) {
        data.peersdb = ai.peers_db_arg;
    } else {
        data.peersdb = defaultDataDir + "peers.db3";
    }
    if (ai.chain_db_given) {
        data.chaindb = ai.chain_db_arg;
    } else {
        data.chaindb = defaultDataDir + "chain.db3";
    }
    if (!std::filesystem::exists(defaultDataDir)) {
        if (!dmp)
            spdlog::info("Crating default directory {}", defaultDataDir);
        std::error_code ec;
        if (!std::filesystem::create_directories(defaultDataDir, ec)) {
            throw std::runtime_error("Cannot create default directory " + defaultDataDir + ": " + ec.message());
        }
    }
    // copy default values
    node.bind = EndpointAddress::parse(ai.bind_arg).value();
    jsonrpc.bind = EndpointAddress::parse(ai.rpc_arg).value();
    peers.connect = {};
    if (ENABLE_DEFAULT_NODE) {
        peers.connect = {
            { "1.92.79.140:9186" },
            { "45.91.203.135:9186" },
            { "93.92.201.8:9186" },
            { "149.102.141.100:9186" },
            { "119.28.71.187:9186" },
            { "135.181.200.100:9186" },
            { "135.181.142.177:9186" },
            { "103.91.16.143:9186" },
            { "101.43.125.67:15806" },
            { "91.107.162.154:9186" },
            { "89.104.71.12:9186" },
            { "68.227.255.200:9186" },
            { "185.255.134.101:9186" },
            { "193.218.118.57:9186" },
            { "185.162.32.61:9186" },
            { "119.17.136.107:9186" },
            { "89.104.69.92:9186" },
            { "74.122.131.1:9186" }
        };
    }

    std::string filename = "config.toml";
    if (!ai.config_given && !std::filesystem::exists(filename)) {
        if (!dmp)
            spdlog::debug("No config.toml file found, using default configuration");
        if (ai.test_given) {
            spdlog::error("No configuration file found.");
            return -1;
        }
    } else {
        if (ai.config_given)
            filename = ai.config_arg;
        if (!dmp)
            spdlog::info("Reading configuration file \"{}\"", filename);
        try {
            // overwrite with config file
            toml::table tbl = toml::parse_file(filename);
            for (auto& [key, val] : tbl) {
                auto t = val.as_table();
                if (key == "db") {
                    for (auto& [k, v] : *t) {
                        if (k == "chain-db")
                            data.chaindb = fetch<std::string>(v);
                        else if (k == "peers-db")
                            data.peersdb = fetch<std::string>(v);
                        else
                            warning_config(k);
                    }
                } else if (key == "jsonrpc") {
                    for (auto& [k, v] : *t) {
                        if (k == "bind")
                            jsonrpc.bind = fetch_endpointaddress(v);
                        else
                            warning_config(k);
                    }
                } else if (key == "node") {
                    for (auto& [k, v] : *t) {
                        if (k == "bind") {
                            node.bind = fetch_endpointaddress(v);
                        } else if (k == "connect") {
                            peers.connect.clear();
                            toml::array& c = array_ref(v);
                            for (auto& e : c) {
                                peers.connect.push_back(fetch_endpointaddress(e));
                            }
                        } else if (k == "leader-key") {
                            node.snapshotSigner = parse_leader_key(fetch<std::string>(v));
                        } else if (k == "enable-ban") {
                            peers.enableBan = fetch<bool>(v);
                        } else if (k == "allow-localhost-ip") {
                            peers.allowLocalhostIp = fetch<bool>(v);
                        } else if (k == "log-communication") {
                            node.logCommunication = fetch<bool>(v);
                        } else
                            warning_config(k);
                    }
                } else {
                    warning_config(key);
                }
            }
            if (ai.test_given) {
                std::cout << "Configuration file \"" + filename + "\" is vaild.\n";
                return 0;
            }
        } catch (const toml::parse_error& err) {
            std::cerr << "Error while parsing file '" << *err.source().path << "':\n"
                      << err.description() << "\n  (" << err.source().begin
                      << ")\n";
            return -1;
        } catch (const std::runtime_error& e) {
            std::cerr << e.what();
            return -1;
        }
    }

    // DB args
    if (ai.chain_db_given)
        data.chaindb = ai.chain_db_arg;
    if (ai.peers_db_given)
        data.peersdb = ai.peers_db_arg;

    // JSONRPC
    if (ai.rpc_given) {
        auto p = EndpointAddress::parse(ai.rpc_arg);
        if (!p) {
            std::cerr << "Bad --rpc option '" << ai.rpc_arg << "'.\n";
            return -1;
        };
        jsonrpc.bind = p.value();
    }

    // Node
    if (ai.bind_given) {
        auto p = EndpointAddress::parse(ai.bind_arg);
        if (!p) {
            std::cerr << "Bad --bind option '" << ai.bind_arg << "'.\n";
            return -1;
        };
        node.bind = p.value();
    }
    if (ai.connect_given) {
        peers.connect = parse_endpoints(ai.connect_arg);
    }

    if (dmp) {
        std::cout << dump();
        return 0;
    }
    return 1;
}

std::string Config::dump()
{
    toml::table tbl;
    tbl.insert_or_assign("jsonrpc", toml::table {
                                        { "bind", jsonrpc.bind.to_string() },
                                    });

    toml::array connect;
    for (auto ea : peers.connect) {
        connect.push_back(ea.to_string());
    }
    tbl.insert_or_assign("node", toml::table { { "bind", node.bind.to_string() }, { "connect", connect }, { "enable-ban", peers.enableBan }, { "allow-localhost-ip", peers.allowLocalhostIp }, { "log-communication", (bool)node.logCommunication } });
    tbl.insert_or_assign("db", toml::table {
                                   { "chain-db", data.chaindb },
                                   { "peers-db", data.peersdb },
                               });
    stringstream ss;
    ss << tbl;
    return ss.str();
}
