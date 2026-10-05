#include "FtpsStorageClient.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace BambuFtps {
namespace {

std::once_flag curl_init_flag;

void ensure_curl_init()
{
    std::call_once(curl_init_flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::string encode_path(std::string const &path)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size() + 16);
    for (unsigned char ch : path) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(hex[(ch >> 4) & 0x0f]);
            out.push_back(hex[ch & 0x0f]);
        }
    }
    return out;
}

std::string curl_error(CURLcode code, char const *buffer)
{
    if (buffer && buffer[0])
        return buffer;
    return curl_easy_strerror(code);
}

void configure(CURL *curl, std::string const &url, std::string const &user,
               std::string const &password, char *error_buffer)
{
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, user.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());

    // Bambu printers use implicit FTPS on 990 and protect the data channel too.
    curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    // Bambu's FTP daemon does not implement MLSD/EPSV reliably and PASV may
    // advertise 0.0.0.0. Force classic passive mode and keep using the
    // control connection's host for the data socket.
    curl_easy_setopt(curl, CURLOPT_FTP_USE_EPSV, 0L);
    curl_easy_setopt(curl, CURLOPT_FTP_SKIP_PASV_IP, 1L);
    curl_easy_setopt(curl, CURLOPT_FTP_FILEMETHOD, CURLFTPMETHOD_SINGLECWD);

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
}

size_t append_to_string(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *out = static_cast<std::string *>(userdata);
    const size_t count = size * nmemb;
    out->append(ptr, count);
    return count;
}

struct TransferContext
{
    DataSink sink;
    Progress progress;
    bool cancelled = false;
};

size_t transfer_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *ctx = static_cast<TransferContext *>(userdata);
    const size_t count = size * nmemb;
    if (count == 0)
        return 0;
    if (ctx->sink && !ctx->sink(ptr, count)) {
        ctx->cancelled = true;
        return 0;
    }
    return count;
}

int transfer_progress(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                      curl_off_t ultotal, curl_off_t ulnow)
{
    auto *ctx = static_cast<TransferContext *>(userdata);
    if (!ctx->progress)
        return 0;
    const curl_off_t total = dltotal > 0 ? dltotal : ultotal;
    const curl_off_t now   = dltotal > 0 ? dlnow : ulnow;
    if (!ctx->progress(static_cast<std::uint64_t>(std::max<curl_off_t>(0, now)),
                       static_cast<std::uint64_t>(std::max<curl_off_t>(0, total)))) {
        ctx->cancelled = true;
        return 1;
    }
    return 0;
}

int month_index(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char *months[] = {
        "jan", "feb", "mar", "apr", "may", "jun",
        "jul", "aug", "sep", "oct", "nov", "dec"
    };
    for (int i = 0; i < 12; ++i)
        if (value == months[i])
            return i;
    return -1;
}

std::time_t timegm_portable(std::tm *value)
{
#ifdef _WIN32
    return _mkgmtime(value);
#else
    return timegm(value);
#endif
}

bool parse_list_line(std::string const &line, Entry &entry)
{
    if (line.size() < 2 || (line[0] != '-' && line[0] != 'd' && line[0] != 'l'))
        return false;

    struct Token { size_t start; size_t length; };
    std::vector<Token> tokens;
    for (size_t i = 0; i < line.size();) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
            ++i;
        if (i >= line.size())
            break;
        const size_t start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])))
            ++i;
        tokens.push_back({start, i - start});
    }
    if (tokens.size() < 9)
        return false;

    auto tok = [&](size_t index) {
        return line.substr(tokens[index].start, tokens[index].length);
    };

    entry.is_dir = line[0] == 'd';
    entry.size = std::strtoull(tok(4).c_str(), nullptr, 10);
    entry.name = line.substr(tokens[8].start);
    const auto arrow = entry.name.find(" -> ");
    if (arrow != std::string::npos)
        entry.name.erase(arrow);
    if (entry.name.empty() || entry.name == "." || entry.name == "..")
        return false;

    const int month = month_index(tok(5));
    const int day = std::atoi(tok(6).c_str());
    const std::string when = tok(7);
    if (month >= 0 && day > 0) {
        std::tm tm{};
        tm.tm_mon = month;
        tm.tm_mday = day;
        const auto colon = when.find(':');
        if (colon != std::string::npos) {
            const std::time_t now = std::time(nullptr);
            std::tm now_tm{};
#ifdef _WIN32
            gmtime_s(&now_tm, &now);
#else
            gmtime_r(&now, &now_tm);
#endif
            tm.tm_year = now_tm.tm_year;
            tm.tm_hour = std::atoi(when.substr(0, colon).c_str());
            tm.tm_min = std::atoi(when.substr(colon + 1).c_str());
            std::time_t candidate = timegm_portable(&tm);
            constexpr std::time_t six_months = 183LL * 24 * 60 * 60;
            if (candidate > now + six_months) {
                --tm.tm_year;
                candidate = timegm_portable(&tm);
            }
            if (candidate > 0)
                entry.mtime = static_cast<std::uint64_t>(candidate);
        } else {
            const int year = std::atoi(when.c_str());
            if (year >= 1970) {
                tm.tm_year = year - 1900;
                const std::time_t candidate = timegm_portable(&tm);
                if (candidate > 0)
                    entry.mtime = static_cast<std::uint64_t>(candidate);
            }
        }
    }
    return true;
}

} // namespace

Client::Client(std::string host, std::string username, std::string password)
    : m_host(std::move(host)), m_username(std::move(username)), m_password(std::move(password))
{
    if (m_username.empty())
        m_username = "bblp";
    ensure_curl_init();
}

std::string Client::url(std::string const &path, bool directory) const
{
    std::string host = m_host;
    if (host.find(':') != std::string::npos && (host.empty() || host.front() != '['))
        host = "[" + host + "]";

    std::string p = path.empty() ? "/" : path;
    if (p.front() != '/')
        p.insert(p.begin(), '/');
    if (directory && p.back() != '/')
        p.push_back('/');
    return "ftps://" + host + ":990" + encode_path(p);
}

std::string Client::list(std::string const &path, std::vector<Entry> &entries) const
{
    entries.clear();
    if (!ready())
        return "FTPS endpoint is incomplete";

    CURL *curl = curl_easy_init();
    if (!curl)
        return "curl_easy_init failed";

    char error_buffer[CURL_ERROR_SIZE]{};
    std::string body;
    configure(curl, url(path, true), m_username, m_password, error_buffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_DIRLISTONLY, 0L);

    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK)
        return curl_error(rc, error_buffer);

    size_t offset = 0;
    while (offset < body.size()) {
        const size_t newline = body.find('\n', offset);
        std::string line = body.substr(offset, newline == std::string::npos ? std::string::npos : newline - offset);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        Entry entry;
        if (parse_list_line(line, entry))
            entries.push_back(std::move(entry));
        if (newline == std::string::npos)
            break;
        offset = newline + 1;
    }
    return {};
}

std::string Client::retrieve(std::string const &path, DataSink const &sink, Progress const &progress) const
{
    if (!ready())
        return "FTPS endpoint is incomplete";

    CURL *curl = curl_easy_init();
    if (!curl)
        return "curl_easy_init failed";

    char error_buffer[CURL_ERROR_SIZE]{};
    TransferContext ctx{sink, progress, false};
    configure(curl, url(path, false), m_username, m_password, error_buffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, transfer_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK)
        return ctx.cancelled ? "cancelled" : curl_error(rc, error_buffer);
    return {};
}

std::string Client::retrieve_range(std::string const &path, std::uint64_t offset,
                                   std::size_t length, DataSink const &sink) const
{
    if (!ready())
        return "FTPS endpoint is incomplete";
    if (length == 0)
        return {};

    CURL *curl = curl_easy_init();
    if (!curl)
        return "curl_easy_init failed";

    char error_buffer[CURL_ERROR_SIZE]{};
    TransferContext ctx{sink, {}, false};
    configure(curl, url(path, false), m_username, m_password, error_buffer);
    const std::uint64_t end = offset + static_cast<std::uint64_t>(length) - 1;
    const std::string range = std::to_string(offset) + "-" + std::to_string(end);
    curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, transfer_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);

    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK)
        return ctx.cancelled ? "cancelled" : curl_error(rc, error_buffer);
    return {};
}

std::string Client::size(std::string const &path, std::uint64_t &size_out) const
{
    size_out = 0;
    if (!ready())
        return "FTPS endpoint is incomplete";

    CURL *curl = curl_easy_init();
    if (!curl)
        return "curl_easy_init failed";

    char error_buffer[CURL_ERROR_SIZE]{};
    configure(curl, url(path, false), m_username, m_password, error_buffer);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);

    const CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_off_t length = -1;
        if (curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length) == CURLE_OK && length >= 0)
            size_out = static_cast<std::uint64_t>(length);
    }
    curl_easy_cleanup(curl);
    return rc == CURLE_OK ? std::string() : curl_error(rc, error_buffer);
}

std::string Client::remove(std::string const &path) const
{
    if (!ready())
        return "FTPS endpoint is incomplete";

    CURL *curl = curl_easy_init();
    if (!curl)
        return "curl_easy_init failed";

    char error_buffer[CURL_ERROR_SIZE]{};
    configure(curl, url("/", true), m_username, m_password, error_buffer);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);

    std::string command = "DELE " + path;
    command.erase(std::remove(command.begin(), command.end(), '\r'), command.end());
    command.erase(std::remove(command.begin(), command.end(), '\n'), command.end());
    curl_slist *commands = nullptr;
    commands = curl_slist_append(commands, command.c_str());
    curl_easy_setopt(curl, CURLOPT_QUOTE, commands);

    const CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(commands);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK ? std::string() : curl_error(rc, error_buffer);
}

std::string Client::upload(std::string const &local_path, std::string const &remote_path, Progress const &progress) const
{
    if (!ready())
        return "FTPS endpoint is incomplete";

    FILE *file = std::fopen(local_path.c_str(), "rb");
    if (!file)
        return "Cannot open local file";

    std::fseek(file, 0, SEEK_END);
    const long long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    CURL *curl = curl_easy_init();
    if (!curl) {
        std::fclose(file);
        return "curl_easy_init failed";
    }

    char error_buffer[CURL_ERROR_SIZE]{};
    TransferContext ctx{{}, progress, false};
    configure(curl, url(remote_path, false), m_username, m_password, error_buffer);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READDATA, file);
    if (length >= 0)
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(length));
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    std::fclose(file);
    if (rc != CURLE_OK)
        return ctx.cancelled ? "cancelled" : curl_error(rc, error_buffer);
    return {};
}

} // namespace BambuFtps
