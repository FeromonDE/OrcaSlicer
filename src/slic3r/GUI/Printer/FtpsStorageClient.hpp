#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace BambuFtps {

struct Entry
{
    std::string name;
    std::uint64_t size = 0;
    std::uint64_t mtime = 0;
    bool is_dir = false;
};

using DataSink = std::function<bool(const void *, std::size_t)>;
using Progress = std::function<bool(std::uint64_t, std::uint64_t)>;

class Client
{
public:
    Client(std::string host, std::string username, std::string password);

    bool ready() const { return !m_host.empty() && !m_password.empty(); }

    std::string list(std::string const &path, std::vector<Entry> &entries) const;
    std::string retrieve(std::string const &path, DataSink const &sink, Progress const &progress = {}) const;
    std::string size(std::string const &path, std::uint64_t &size_out) const;
    std::string remove(std::string const &path) const;
    std::string upload(std::string const &local_path, std::string const &remote_path, Progress const &progress = {}) const;

private:
    std::string url(std::string const &path, bool directory = false) const;

    std::string m_host;
    std::string m_username;
    std::string m_password;
};

} // namespace BambuFtps
