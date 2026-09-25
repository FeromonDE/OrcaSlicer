#include "PrinterFileSystem.h"
#include "FtpsStorageClient.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/GUI/I18N.hpp"

#include "../../Utils/NetworkAgent.hpp"
#include "../BitmapCache.hpp"

#include <boost/algorithm/hex.hpp>
#include <boost/endian/conversion.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/detail/md5.hpp>
#include <boost/regex.hpp>

#include <wx/mstream.h>

#include "nlohmann/json.hpp"

#include <cctype>
#include <cstring>

#ifndef NDEBUG
//#define PRINTER_FILE_SYSTEM_TEST
#endif

std::string last_system_error() {
    return Slic3r::decode_path(std::error_code(
#ifdef _WIN32
        GetLastError(),
#else
        errno,
#endif
        std::system_category()).message().c_str());
}

wxDEFINE_EVENT(EVT_STATUS_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_MODE_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_FILE_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_SELECT_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_THUMBNAIL, wxCommandEvent);
wxDEFINE_EVENT(EVT_DOWNLOAD, wxCommandEvent);
wxDEFINE_EVENT(EVT_RAMDOWNLOAD, wxCommandEvent);
wxDEFINE_EVENT(EVT_MEDIA_ABILITY_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_UPLOADING, wxCommandEvent);
wxDEFINE_EVENT(EVT_UPLOAD_CHANGED, wxCommandEvent);

wxDEFINE_EVENT(EVT_FILE_CALLBACK, wxCommandEvent);

static wxBitmap default_thumbnail;

static constexpr int STORAGE_CACHE_VERSION = 1;

static std::string storage_cache_hash(std::string const &value)
{
    boost::uuids::detail::md5 md5;
    md5.process_bytes(value.data(), value.size());
    boost::uuids::detail::md5::digest_type digest;
    md5.get_digest(digest);
    for (int i = 0; i < 4; ++i)
        digest[i] = boost::endian::endian_reverse(digest[i]);

    std::string result;
    auto bytes = reinterpret_cast<char const *>(&digest[0]);
    boost::algorithm::hex(bytes, bytes + sizeof(digest), std::back_inserter(result));
    return result;
}

static std::string ftps_join_path(std::string const &base, std::string const &name)
{
    if (base.empty() || base == "/")
        return "/" + name;
    return base.back() == '/' ? base + name : base + "/" + name;
}

static std::string ftps_subtree(std::string const &prefix, std::string const &type)
{
    if (type == "timelapse")
        return ftps_join_path(prefix, "timelapse");
    if (type == "video")
        return ftps_join_path(prefix, "ipcam");
    return prefix.empty() ? "/" : prefix;
}

static bool ftps_keep_file(std::string const &type, std::string const &name)
{
    auto ends_with_ci = [&name](char const *suffix) {
        const size_t n = std::strlen(suffix);
        if (name.size() < n)
            return false;
        return std::equal(name.end() - n, name.end(), suffix,
            [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            });
    };
    if (type == "timelapse")
        return ends_with_ci(".mp4") || ends_with_ci(".avi");
    if (type == "video")
        return ends_with_ci(".mp4");
    if (type == "model")
        return ends_with_ci(".3mf") || ends_with_ci(".gcode") || ends_with_ci(".gcode.3mf");
    return true;
}

static std::string ftps_base_path(std::string const &path)
{
    const size_t hash = path.find('#');
    return hash == std::string::npos ? path : path.substr(0, hash);
}

static std::string ftps_sub_path(std::string const &path)
{
    const size_t hash = path.find('#');
    if (hash == std::string::npos)
        return {};
    std::string result = path.substr(hash + 1);
    while (!result.empty() && result.front() == '/')
        result.erase(result.begin());
    return result;
}

static bool ftps_extract_zip_entry(std::string const &archive_data, std::string const &entry_name, std::string &output)
{
    output.clear();
    if (archive_data.empty() || entry_name.empty())
        return false;

    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, archive_data.data(), archive_data.size(), 0))
        return false;

    int index = mz_zip_reader_locate_file(&archive, entry_name.c_str(), nullptr, 0);
    if (index < 0) {
        mz_zip_reader_end(&archive);
        return false;
    }

    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive, index, &stat)) {
        mz_zip_reader_end(&archive);
        return false;
    }

    output.resize(static_cast<size_t>(stat.m_uncomp_size));
    const bool ok = stat.m_uncomp_size == 0 ||
                    mz_zip_reader_extract_to_mem(&archive, index, output.data(), output.size(), 0);
    mz_zip_reader_end(&archive);
    if (!ok)
        output.clear();
    return ok;
}

static std::string storage_md5_hex(boost::uuids::detail::md5 &md5)
{
    boost::uuids::detail::md5::digest_type digest;
    md5.get_digest(digest);
    for (int i = 0; i < 4; ++i)
        digest[i] = boost::endian::endian_reverse(digest[i]);
    std::string result;
    const auto bytes = reinterpret_cast<char const *>(&digest[0]);
    boost::algorithm::hex(bytes, bytes + sizeof(digest), std::back_inserter(result));
    return result;
}

static std::map<int, std::string> error_messages = {
     {PrinterFileSystem::ERROR_PIPE, L("Reconnecting the printer, the operation cannot be completed immediately, please try again later.")},
     {PrinterFileSystem::ERROR_RES_BUSY, L("The device cannot handle more conversations. Please retry later.")},
     {PrinterFileSystem::ERROR_TIME_OUT, L("Timeout, please try again.")},
     {PrinterFileSystem::FILE_NO_EXIST, L("File does not exist.")},
     {PrinterFileSystem::FILE_CHECK_ERR, L("File checksum error. Please retry.")},
     {PrinterFileSystem::FILE_TYPE_ERR, L("Not supported on the current printer version.")},
     {PrinterFileSystem::STORAGE_UNAVAILABLE, L("Please check if the storage is inserted into the printer.\nIf it still cannot be read, you can try formatting the storage.")},
     {PrinterFileSystem::API_VERSION_UNSUPPORT, L("The firmware version of the printer is too low. Please update the firmware and try again.")},
     {PrinterFileSystem::FILE_EXIST, L("The file already exists, do you want to replace it?")},
     {PrinterFileSystem::STORAGE_SPACE_NOT_ENOUGH, L("Insufficient storage space, please clear the space and try again.")},
     {PrinterFileSystem::FILE_CREATE_ERR, L("File creation failed, please try again.")},
     {PrinterFileSystem::FILE_WRITE_ERR, L("File write failed, please try again.")},
     {PrinterFileSystem::MD5_COMPARE_ERR, L("MD5 verification failed, please try again.")},
     {PrinterFileSystem::FILE_RENAME_ERR, L("File renaming failed, please try again.")},
     {PrinterFileSystem::SEND_ERR, L("File upload failed, please try again.")}
};

struct StaticBambuLib : BambuLib {
    static StaticBambuLib &get(BambuLib * copy = nullptr);
    static int Fake_Bambu_Create(Bambu_Tunnel*, char const*) { return -2; }
    static void reset();
    static void release();
private:
    std::vector<BambuLib *> copies_;
};

PrinterFileSystem::PrinterFileSystem()
    : BambuLib(StaticBambuLib::get(this))
{
    if (!default_thumbnail.IsOk()) {
        default_thumbnail = *Slic3r::GUI::BitmapCache().load_svg("printer_file", 0, 0);
#ifdef __APPLE__
        default_thumbnail = wxBitmap(default_thumbnail.ConvertToImage(), -1, 1);
#endif
    }
    m_session.owner = this;
#ifdef PRINTER_FILE_SYSTEM_TEST
    auto time = wxDateTime::Now();
    wxString path = "D:\\work\\pic\\";
    for (int i = 0; i < 10; ++i) {
        auto name = wxString::Format(L"gcode-%02d.3mf", i + 1);
        m_file_list.push_back({name.ToUTF8().data(), "", time.GetTicks(), 26937, i < 5 ? FF_DOWNLOAD : 0, default_thumbnail});
        std::ifstream ifs((path + name).ToUTF8().data(), std::ios::binary);
        if (ifs)
            ParseThumbnail(m_file_list.back(), ifs);
        time.Add(wxDateSpan::Days(-1));
    }
    m_file_list.swap(m_file_list_cache[{F_MODEL, ""}]);
    time = wxDateTime::Now();
    for (int i = 0; i < 100; ++i) {
        auto name = wxString::Format(L"img-%03d.jpg", i + 1);
        wxImage im(path + name);
        m_file_list.push_back({name.ToUTF8().data(), "", time.GetTicks(), 26937, i < 20 ? FF_DOWNLOAD : 0, i > 3 ? im : default_thumbnail});
        time.Add(wxDateSpan::Days(-1));
    }
    m_file_list[0].thumbnail = default_thumbnail;
    m_file_list.swap(m_file_list_cache[{F_TIMELAPSE, ""}]);
#endif
}

PrinterFileSystem::~PrinterFileSystem()
{
    if (m_recv_thread.joinable())
        m_recv_thread.detach();
}

void PrinterFileSystem::SetCacheScope(std::string const &printer_id)
{
    m_cache_scope = printer_id.empty() ? std::string() : storage_cache_hash(printer_id);
    BOOST_LOG_TRIVIAL(info) << "[StorageCache] scope=" << (m_cache_scope.empty() ? "disabled" : m_cache_scope);
}

void PrinterFileSystem::SetUseFtps(bool enabled)
{
    m_use_ftps = enabled;
    BOOST_LOG_TRIVIAL(info) << "[StorageTransport] mode=" << (m_use_ftps ? "FTPS:990" : "native:6000");
}

void PrinterFileSystem::SetFtpsEndpoint(std::string const &host, std::string const &user, std::string const &password)
{
    {
        boost::unique_lock lock(m_ftps_mutex);
        m_ftps_host = host;
        m_ftps_user = user.empty() ? "bblp" : user;
        m_ftps_password = password;
        m_ftps_prefix.clear();
        m_ftps_storage_label.clear();
        m_ftps_archive_cache.clear();
    }

    if (host.empty() || password.empty()) {
        m_last_error = ERROR_PIPE;
        m_status = Status::Failed;
        SendChangedEvent(EVT_STATUS_CHANGED, m_status,
            "FTPS requires the printer LAN IP address and access code.", ERROR_PIPE);
        return;
    }

    m_last_error = 0;
    m_status = Status::ListSyncing;
    SendChangedEvent(EVT_STATUS_CHANGED, m_status);
}

void PrinterFileSystem::SetFileType(FileType type, std::string const &storage)
{
    if (m_file_type == type && m_file_storage == storage)
        return;
    bool storage_only_changed = (m_file_type == type && m_file_storage != storage);
    SelectAll(false);
    assert(m_file_list_cache[std::make_pair(m_file_type, m_file_storage)].empty());
    m_file_list.swap(m_file_list_cache[{m_file_type, m_file_storage}]);
    std::swap(m_file_type, type);
    m_file_storage = storage;
    if (storage_only_changed)
        m_file_list_cache[{m_file_type, m_file_storage}].clear();
    m_file_list.swap(m_file_list_cache[{m_file_type, m_file_storage}]);
    m_lock_start = m_lock_end = 0;
    BuildGroups();
    UpdateGroupSelect();
    SendChangedEvent(EVT_FILE_CHANGED);
    if (type == F_INVALID_TYPE)
        return;
    if (m_use_ftps) {
        bool ready = false;
        {
            boost::unique_lock lock(m_ftps_mutex);
            ready = !m_ftps_host.empty() && !m_ftps_password.empty();
        }
        if (!ready || m_stopped)
            return;
        m_status = Status::ListSyncing;
        SendChangedEvent(EVT_STATUS_CHANGED, m_status);
        return;
    }
    if (m_session.tunnel == nullptr)
        return;
    m_status = Status::ListSyncing;
    SendChangedEvent(EVT_STATUS_CHANGED, m_status);
}

void PrinterFileSystem::SetGroupMode(GroupMode mode)
{
    if (this->m_group_mode == mode)
        return;
    this->m_group_mode = mode;
    m_lock_start = m_lock_end = 0;
    UpdateGroupSelect();
    SendChangedEvent(EVT_MODE_CHANGED);
}

size_t PrinterFileSystem::EnterSubGroup(size_t index)
{
    if (m_group_mode == G_NONE)
        return index;
    index = m_group_mode == G_YEAR ? m_group_year[index] : m_group_month[index];
    SetGroupMode((GroupMode)(m_group_mode - 1));
    return index;
}

void PrinterFileSystem::ListAllFiles()
{
    json req;
    char const * types[] {"timelapse","video", "model" };
    req["type"] = types[m_file_type];
    if (!m_file_storage.empty())
        req["storage"] = m_file_storage;
    req["api_version"] = 2;
    req["notify"] = "DETAIL";
    BOOST_LOG_TRIVIAL(info) << "[StorageTrace] LIST_INFO request"
                               << " type=" << m_file_type
                               << " storage=" << m_file_storage
                               << " req=" << req.dump();
    SendRequest<FileList>(LIST_INFO, req, [type = m_file_type](json const& resp, FileList & list, auto) -> int {
        json files = resp["file_lists"];
        for (auto& f : files) {
            std::string     name = f["name"];
            std::string     path = f.value("path", "");
            time_t          time = f.value("time", 0);
            boost::uint64_t size = f["size"];
            if (type > F_TIMELAPSE && path.empty()) // Fix old printer that always return timelapses
                return FILE_TYPE_ERR;
            File            ff   = {name, path, time, size, 0};
            list.push_back(ff);
        }
        return 0;
    }, [this, type = m_file_type](int result, FileList list) {
        BOOST_LOG_TRIVIAL(info) << "[StorageTrace] LIST_INFO callback"
                                   << " type=" << type
                                   << " current_type=" << m_file_type
                                   << " storage=" << m_file_storage
                                   << " result=" << result
                                   << " files=" << list.size();
        if (result != 0) {
            m_last_error = result;
            m_status = Status::Failed;
            m_file_list.clear();
            BuildGroups();
            UpdateGroupSelect();
            SendChangedEvent(EVT_STATUS_CHANGED, m_status, "", result);
            SendChangedEvent(EVT_FILE_CHANGED);
            return 0;
        }
        if (type != m_file_type)
            return 0;
        m_file_list.swap(list);
        for (auto & file : m_file_list)
            file.thumbnail = default_thumbnail;
        std::sort(m_file_list.begin(), m_file_list.end());
        auto iter1 = m_file_list.begin();
        auto end1  = m_file_list.end();
        auto iter2 = list.begin();
        auto end2  = list.end();
        while (iter1 != end1 && iter2 != end2) {
            if (*iter1 < *iter2) {
                ++iter1;
            } else if (*iter2 < *iter1) {
                ++iter2;
            } else {
                if (iter1->path == iter2->path && iter1->name == iter2->name) {
                    iter1->thumbnail = iter2->thumbnail;
                    iter1->flags     = iter2->flags;
                    if (!iter1->thumbnail.IsOk())
                        iter1->flags &= ~FF_THUMNAIL;
                    iter1->download   = iter2->download;
                    iter1->local_path = iter2->local_path;
                    iter1->metadata   = iter2->metadata;
                }
                ++iter1;
                ++iter2;
            }
        }
        BuildGroups();
        UpdateGroupSelect();
        m_last_error = 0;
        m_status = Status::ListReady;
        SendChangedEvent(EVT_STATUS_CHANGED, m_status);
        SendChangedEvent(EVT_FILE_CHANGED);
        if ((m_task_flags & FF_DOWNLOAD) == 0)
            DownloadNextFile();
        return 0;
    });
}

void PrinterFileSystem::DeleteFiles(size_t index)
{
    if (index == size_t(-1)) {
        size_t n = 0;
        for (size_t i = 0; i < m_file_list.size(); ++i) {
            auto &file = m_file_list[i];
            if ((file.flags & FF_SELECT) != 0 && (file.flags & FF_DELETED) == 0) {
                file.flags |= FF_DELETED;
                ++n;
            }
        }
        if (n == 0) return;
    } else {
        if (index >= m_file_list.size())
            return;
        auto &file = m_file_list[index];
        if ((file.flags & FF_DELETED) != 0)
            return;
        file.flags |= FF_DELETED;
    }
    if ((m_task_flags & FF_DELETED) == 0)
        DeleteFilesContinue();
}

struct PrinterFileSystem::Download : Progress
{
    size_t                      index;
    std::string                 name;
    std::string                 path;
    std::string                 local_path;
    std::string                 error;
    boost::filesystem::ofstream ofs;
    boost::uuids::detail::md5   boost_md5;
};

struct PrinterFileSystem::Upload : Progress
{
    std::string                 error;
    boost::uint32_t             frag_id{0};
    MD5_CTX                     ctx;
    boost::filesystem::ifstream ifs;
};


void PrinterFileSystem::GetPickImages(const std::vector<std::string> &local_paths, const std::vector<std::string> &targetpaths)
{
    m_download_states.clear();

    GetPickImage(1, local_paths[0], targetpaths[0]);
    GetPickImage(2, local_paths[1], targetpaths[1]);
    GetPickImage(3, local_paths[2], targetpaths[2]);

}

void PrinterFileSystem::GetPickImage(int id, const std::string &local_path, const std::string &targetpath)
{
    json j;

    j["sequence_id"]   = id;
    j["version"]       = 1;
    j["peer_host"]     = "studio";
    j["command"]       = "get_project_file";
    j["file_rel_path"] = targetpath;

    std::string param = j.dump();

    DownloadRamFile(16, local_path, param);
}


void PrinterFileSystem::DownloadRamFile(int index, const std::string &local_path, const std::string & param)
{
    std::shared_ptr<Download> download(new Download);
    download->local_path = local_path;

    json req;
    req["path"]              = "mem:/" + std::to_string(index);
    req["offset"] = 0;
    req["mem_dl_param_size"] = param.size();

    m_download_seq = SendRequest<Progress>(
        FILE_DOWNLOAD, req,
        [download](json const &resp, Progress &prog, unsigned char const *data) -> int {
            size_t size = resp.value("size", 0);
            prog.size   = resp["offset"];
            prog.total  = resp["total"];

            if (resp.contains("mem_dl_param_size")) {
                size_t s = resp["mem_dl_param_size"].get<size_t>();
                std::string json_str(reinterpret_cast<const char *>(data), s);
                // OutputDebugStringA(json_str.c_str());
                // OutputDebugStringA("\n");
                json        mem_dl_json = json::parse(json_str);
                //  download->mem_dl_param_size = size;
                if (!mem_dl_json.contains("result") || mem_dl_json["result"] == 1 ) {
                        wxLogWarning("Download failed: result = 1");
                    return ERROR_JSON;
                    }
                if(mem_dl_json.contains("size") && mem_dl_json["size"] == 0 )
                    return FILE_SIZE_ERR;

                return CONTINUE;
            }

            if (prog.size == 0 ) {
                download->ofs.open(download->local_path, std::ios::binary);
                if (!download->ofs) {
                    download->error = last_system_error();
                    wxLogWarning("DownloadImageFromRam open error: %s\n", wxString::FromUTF8(download->error));
                    return FILE_OPEN_ERR;
                }
            }

            download->ofs.write(reinterpret_cast<const char *>(data), size);
            if (!download->ofs) {
                download->error = last_system_error();
                wxLogWarning("DownloadImageFromRam write error: %s\n", wxString::FromUTF8(download->error));
                return FILE_READ_WRITE_ERR;
            }

            download->boost_md5.process_bytes(data, size);

            prog.size += size;
            download->total = prog.total;
            download->size  = prog.size;

            if (prog.size < prog.total) {
                return 0;
            }
            download->ofs.close();

            std::string                     md5 = resp["file_md5"];
            boost::uuids::detail::md5::digest_type digest;
            download->boost_md5.get_digest(digest);
            for (int i = 0; i < 4; ++i) digest[i] = boost::endian::endian_reverse(digest[i]);
            std::string str_md5;
            const auto  char_digest = reinterpret_cast<const char *>(&digest[0]);
            boost::algorithm::hex(char_digest, char_digest + sizeof(digest), std::back_inserter(str_md5));
            if (!boost::iequals(str_md5, md5)) {
                wxLogWarning("DownloadImageFromRam checksum error: %s != %s\n", str_md5, md5);
                boost::system::error_code ec;
                boost::filesystem::rename(download->local_path, download->local_path + ".tmp", ec);
                return FILE_CHECK_ERR;
            }
            return SUCCESS;
        },

        [this, download](int result, Progress const &data) {
            //OutputDebugStringA(std::to_string(result).c_str());
            //OutputDebugStringA("\n");
            if (result == CONTINUE) { return; }
            std::string msg;
            if (result == SUCCESS) {
                if (std::filesystem::exists(download->local_path)) {
                    m_download_states.emplace_back(true);
                    BOOST_LOG_TRIVIAL(info) <<"DownloadImageFromRam finished: " << download->local_path << "result = " << result;
                }else{
                    m_download_states.emplace_back(false);
                    BOOST_LOG_TRIVIAL(warning) <<"DownloadImageFromRam finished, but file not exist: " << download->local_path << "result = " << result;
                }
            } else if (result != CONTINUE) {
                m_download_states.emplace_back(false);
                BOOST_LOG_TRIVIAL(warning) << "DownloadImageFromRam failed: " << download->error << "result = " << result;
            }

            if(m_download_states.size() == 3){
                if(m_download_states[0] && m_download_states[1] && m_download_states[2]){
                    SendChangedEvent(EVT_RAMDOWNLOAD, SUCCESS);
                }else{
                    // FILE_NO_EXIST is not really error_code
                    SendChangedEvent(EVT_RAMDOWNLOAD, FILE_NO_EXIST);
                }
            }else{
                 BOOST_LOG_TRIVIAL(warning) << "m_download_states current size is : " << m_download_states.size();
            }
        },param);
}

void PrinterFileSystem::SendExistedFile(){
    SendChangedEvent(EVT_RAMDOWNLOAD, SUCCESS);
}
void PrinterFileSystem::SendConnectFail(){
    SendChangedEvent(EVT_RAMDOWNLOAD, ERROR_PIPE);
}


void PrinterFileSystem::DownloadFiles(size_t index, std::string const &path)
{
    if (index == (size_t) -1) {
        size_t n = 0;
        for (size_t i = 0; i < m_file_list.size(); ++i) {
            auto &file = m_file_list[i];
            if ((file.flags & FF_SELECT) == 0) continue;
            if ((file.flags & FF_DOWNLOAD) != 0 && file.DownloadProgress() >= -1) continue;
            file.flags |= FF_DOWNLOAD;
            std::shared_ptr<Download> download(new Download);
            download->progress = -1;
            download->local_path = (boost::filesystem::path(path) / file.name).string();
            file.download = download;
            ++n;
        }
        if (n == 0) return;
    } else {
        if (index >= m_file_list.size())
            return;
        auto &file = m_file_list[index];
        if ((file.flags & FF_DOWNLOAD) != 0 && file.DownloadProgress() >= -1)
            return;
        file.flags |= FF_DOWNLOAD;
        std::shared_ptr<Download> download(new Download);
        download->progress   = -1;
        download->local_path = (boost::filesystem::path(path) / file.name).string();
        file.download        = download;
    }
    boost::filesystem::create_directories(path);
    if ((m_task_flags & FF_DOWNLOAD) == 0)
        DownloadNextFile();
}





void PrinterFileSystem::DownloadCheckFiles(std::string const &path)
{
    for (size_t i = 0; i < m_file_list.size(); ++i) {
        auto &file = m_file_list[i];
        if ((file.flags & FF_DOWNLOAD) != 0 && file.download) continue;
        auto path2 = boost::filesystem::path(path) / file.name;
        boost::system::error_code ec;
        if (boost::filesystem::file_size(path2, ec) == file.size) {
            file.flags |= FF_DOWNLOAD;
            file.local_path = path2.string();
        }
    }
}

bool PrinterFileSystem::DownloadCheckFile(size_t index)
{
    if (index >= m_file_list.size()) return false;
    auto &file = m_file_list[index];
    if ((file.flags & FF_DOWNLOAD) == 0 || file.local_path.empty())
        return false;
    if (!boost::filesystem::exists(file.local_path)) {
        file.flags &= ~FF_DOWNLOAD;
        file.local_path.clear();
        SendChangedEvent(EVT_DOWNLOAD, index, file.local_path);
        return false;
    }
    return true;
}

void PrinterFileSystem::DownloadCancel(size_t index)
{
    if (index == (size_t) -1) return;
    if (index >= m_file_list.size()) return;
    auto &file = m_file_list[index];
    if ((file.flags & FF_DOWNLOAD) == 0 || !file.download) return;
    if (file.DownloadProgress() >= 0)
        CancelRequest(m_download_seq);
    else
        file.flags &= ~FF_DOWNLOAD, file.download.reset();
}

void PrinterFileSystem::FetchModel(size_t index, std::function<void(int, std::string const &)> callback)
{
    if (m_task_flags & FF_FETCH_MODEL)
        return;
    json req;
    json arr;
    if (index == (size_t) -1) return;
    if (index >= m_file_list.size()) return;
    auto &file = m_file_list[index];
    arr.push_back(file.path + "#_rels/.rels");
    arr.push_back(file.path + "#3D/3dmodel.model");
    arr.push_back(file.path + "#Metadata/model_settings.config");
    arr.push_back(file.path + "#Metadata/slice_info.config");
    arr.push_back(file.path + "#Metadata/project_settings.config");
    for (auto & meta : file.metadata) {
        if (boost::algorithm::starts_with(meta.first, "plate_thumbnail_"))
            arr.push_back(file.path + "#" + meta.second);
    }
    req["paths"] = arr;
    req["zip"] = true;
    m_task_flags |= FF_FETCH_MODEL;
    std::shared_ptr<std::string> file_data(new std::string());
    m_fetch_model_seq = SendRequest<Void>(
        SUB_FILE, req,
        [file_data](json const &resp, Void &, unsigned char const *data) -> int {
            // in work thread, continue recv
            // receive data
            boost::uint32_t size      = resp["size"];
            if (size > 0) {
                *file_data += std::string((char *) data, size);
            }
            return 0;
        },
        [this, file_data, callback](int result, Void const &) {
            if (result == CONTINUE) return;
            m_task_flags &= ~FF_FETCH_MODEL;
            if (result != 0) {
                auto iter = error_messages.find(result);
                if (iter != error_messages.end())
                    *file_data = _u8L(iter->second.c_str());
                else
                    file_data->clear();
            }
            callback(result, *file_data);
        });
}

void PrinterFileSystem::FetchModelCancel()
{
    if ((m_task_flags & FF_FETCH_MODEL) == 0) return;
    CancelRequests2({m_fetch_model_seq});
}

size_t PrinterFileSystem::GetCount() const
{
    if (m_group_mode == G_NONE)
        return m_file_list.size();
    return m_group_mode == G_YEAR ? m_group_year.size() : m_group_month.size();
}

int PrinterFileSystem::File::DownloadProgress() const { return download ? download->progress : !local_path.empty() ? 100 : -2; }

std::string PrinterFileSystem::File::Title() const { return Metadata("Title", ""); }

std::string PrinterFileSystem::File::Metadata(std::string const &key, std::string const &dflt) const
{
    auto iter = metadata.find(key);
    return iter == metadata.end() || iter->second.empty() ? dflt : iter->second;
}

PrinterFileSystem::UploadFile::~UploadFile()
{
    if (upload && upload->ifs.is_open()) {
        upload->ifs.close();
    }
}

size_t PrinterFileSystem::GetIndexAtTime(boost::uint32_t time)
{
    auto   iter = std::upper_bound(m_file_list.begin(), m_file_list.end(), File{"", "", time});
    size_t n = std::distance(m_file_list.begin(), iter) - 1;
    if (m_group_mode == G_NONE) {
        return n;
    }
    auto & group = m_group_mode == G_YEAR ? m_group_year : m_group_month;
    auto iter2 = std::upper_bound(group.begin(), group.end(), n);
    return std::distance(group.begin(), iter2) - 1;
}

void PrinterFileSystem::ToggleSelect(size_t index)
{
    if (m_group_mode != G_NONE) {
        size_t beg = m_group_mode == G_YEAR ? m_group_month[m_group_year[index]] : m_group_month[index];
        size_t end_month = m_group_mode == G_YEAR ? ((index + 1) < m_group_year.size() ? m_group_year[index + 1] : m_group_month.size()) : index + 1;
        size_t end       = end_month < m_group_month.size() ? m_group_month[end_month] : m_file_list.size();
        if ((m_group_flags[index] & FF_SELECT) == 0) {
            for (int i = beg; i < end; ++i) {
                if ((m_file_list[i].flags & FF_SELECT) == 0) {
                    m_file_list[i].flags |= FF_SELECT;
                    ++m_select_count;
                }
            }
            m_group_flags[index] |= FF_SELECT;
        } else {
            for (int i = beg; i < end; ++i) {
                if (m_file_list[i].flags & FF_SELECT) {
                    m_file_list[i].flags &= ~FF_SELECT;
                    --m_select_count;
                }
            }
            m_group_flags[index] &= ~FF_SELECT;
        }
    } else if (index < m_file_list.size()) {
        m_file_list[index].flags ^= FF_SELECT;
        if (m_file_list[index].flags & FF_SELECT)
            ++m_select_count;
        else
            --m_select_count;
    }
    SendChangedEvent(EVT_SELECT_CHANGED, m_select_count);
}

void PrinterFileSystem::SelectAll(bool select)
{
    if (select) {
        for (auto &f : m_file_list) f.flags |= FF_SELECT;
        m_select_count = m_file_list.size();
        for (auto &s : m_group_flags) s |= FF_SELECT;
    } else {
        for (auto &f : m_file_list) f.flags &= ~FF_SELECT;
        m_select_count = 0;
        for (auto &s : m_group_flags) s &= ~FF_SELECT;
    }
    SendChangedEvent(EVT_SELECT_CHANGED, m_select_count);
}

size_t PrinterFileSystem::GetSelectCount() const { return m_select_count; }

void PrinterFileSystem::SetFocusRange(size_t start, size_t count)
{
    m_lock_start = start;
    m_lock_end = start + count;
    if (!m_stopped && (m_task_flags & FF_THUMNAIL) == 0)
        UpdateFocusThumbnail();
}

PrinterFileSystem::File const &PrinterFileSystem::GetFile(size_t index)
{
    if (m_group_mode == G_NONE)
        return m_file_list[index];
    if (m_group_mode == G_YEAR) index = m_group_year[index];
    return m_file_list[m_group_month[index]];
}

PrinterFileSystem::File const &PrinterFileSystem::GetFile(size_t index, bool &select)
{
    if (m_group_mode == G_NONE) {
        select = m_file_list[index].IsSelect();
        return m_file_list[index];
    }
    select = m_group_flags[index] & FF_SELECT;
    if (m_group_mode == G_YEAR)
        index = m_group_year[index];
    return m_file_list[m_group_month[index]];
}

void PrinterFileSystem::Attached()
{
    if (m_use_ftps)
        return;
    boost::unique_lock lock(m_mutex);
    m_recv_thread = boost::thread([w = weak_from_this()] {
        boost::shared_ptr<PrinterFileSystem> s = w.lock();
        if (s) s->RecvMessageThread();
    });
}

void PrinterFileSystem::Start()
{
    {
        boost::unique_lock l(m_mutex);
        if (!m_stopped) return;
        m_stopped = false;
        if (!m_use_ftps) {
            m_cond.notify_all();
            return;
        }
    }
    m_status = Status::Initializing;
    SendChangedEvent(EVT_STATUS_CHANGED, m_status);
}

void PrinterFileSystem::Retry()
{
    {
        boost::unique_lock l(m_mutex);
        m_stopped = false;
        if (!m_use_ftps) {
            m_cond.notify_all();
            return;
        }
    }
    m_status = Status::Initializing;
    SendChangedEvent(EVT_STATUS_CHANGED, m_status);
}

void PrinterFileSystem::SetUrl(std::string const &url)
{
    if (m_use_ftps)
        return;
    boost::unique_lock l(m_mutex);
    m_messages.push_back(url);
    m_cond.notify_all();
}

void PrinterFileSystem::Stop(bool quit)
{
    if (m_use_ftps) {
        {
            boost::unique_lock l(m_mutex);
            if (quit)
                m_session.owner = nullptr;
            else if (m_stopped)
                return;
            m_stopped = true;
        }
        boost::unique_lock lock(m_ftps_mutex);
        m_ftps_cancelled.insert(m_ftps_active.begin(), m_ftps_active.end());
        return;
    }

    boost::unique_lock l(m_mutex);
    if (quit) {
        m_session.owner = nullptr;
    } else if (m_stopped) {
        return;
    }
    m_stopped = true;
    m_cond.notify_all();
}

void PrinterFileSystem::SetUploadFile(const std::string &path, const std::string &name, const std::string &select_storage)
{
    boost::unique_lock l(m_mutex);
    if (!m_upload_file) {
        m_upload_file = std::make_unique<UploadFile>();
    }
    m_upload_file->path           = path;
    m_upload_file->name           = name;
    m_upload_file->select_storage = select_storage;
}

void PrinterFileSystem::BuildGroups()
{
    m_group_year.clear();
    m_group_month.clear();
    if (m_file_list.empty())
        return;
    wxDateTime t = wxDateTime((time_t) m_file_list.front().time);
    m_group_year.push_back(0);
    m_group_month.push_back(0);
    for (size_t i = 0; i < m_file_list.size(); ++i) {
        wxDateTime s = wxDateTime((time_t) m_file_list[i].time);
        if (s.GetYear() != t.GetYear()) {
            m_group_year.push_back(m_group_month.size());
            m_group_month.push_back(i);
        } else if (s.GetMonth() != t.GetMonth()) {
            m_group_month.push_back(i);
        }
        t = s;
    }
}

void PrinterFileSystem::UpdateGroupSelect()
{
    m_group_flags.clear();
    int beg = 0;
    if (m_group_mode != G_NONE) {
        auto group = m_group_mode == G_YEAR ? m_group_year : m_group_month;
        if (m_group_mode == G_YEAR)
            for (auto &g : group) g = m_group_month[g];
        m_group_flags.resize(group.size(), FF_SELECT);
        for (int i = 0; i < m_file_list.size(); ++i) {
            if ((m_file_list[i].flags & FF_SELECT) == 0) {
                auto iter = std::upper_bound(group.begin(), group.end(), i);
                m_group_flags[iter - group.begin() - 1] &= ~FF_SELECT;
                if (iter == group.end()) break;
                i = *iter - 1; // start from next group
            }
        }
    }
}

void PrinterFileSystem::DeleteFilesContinue()
{
    std::vector<size_t> indexes;
    std::vector<std::string> names;
    std::vector<std::string> paths;
    for (size_t i = 0; i < m_file_list.size(); ++i)
        if ((m_file_list[i].flags & FF_DELETED) && !m_file_list[i].name.empty()) {
            indexes.push_back(i);
            auto &file = m_file_list[i];
            if (file.path.empty())
                names.push_back(file.name);
            else
                paths.push_back(file.path);
            if (names.size() >= 64 || paths.size() >= 64)
                break;
        }
    m_task_flags &= ~FF_DELETED;
    if (names.empty() && paths.empty())
        return;
    json req;
    json arr;
    if (paths.empty()) {
        for (auto &name : names) arr.push_back(name);
        req["delete"] = arr;
    } else {
        for (auto &path : paths) arr.push_back(path);
        req["paths"] = arr;
    }
    m_task_flags |= FF_DELETED;
    auto type = std::make_pair(m_file_type, m_file_storage);
    SendRequest<Void>(
        FILE_DEL, req, nullptr,
        [indexes, type, names = paths.empty() ? names : paths, bypath = !paths.empty(), this](int, Void const &) {
            // TODO:
            for (size_t i = indexes.size() - 1; i != size_t(-1); --i)
                FileRemoved(type, indexes[i], names[i], bypath);
            SendChangedEvent(EVT_FILE_CHANGED, indexes.size());
            DeleteFilesContinue();
        });
}

void PrinterFileSystem::DownloadNextFile()
{
    size_t index = size_t(-1);
    for (size_t i = 0; i < m_file_list.size(); ++i) {
        if (m_file_list[i].IsDownload() && m_file_list[i].DownloadProgress() == -1) {
            index = i;
            break;
        }
    }
    m_task_flags &= ~FF_DOWNLOAD;
    if (index >= m_file_list.size())
        return;
    auto &file = m_file_list[index];
    json req;
    if (file.path.empty())
        req["file"] = file.name;
    else
        req["path"] = file.path;
    SendChangedEvent(EVT_DOWNLOAD, index, m_file_list[index].name);
    std::shared_ptr<Download> download(m_file_list[index].download);
    download->index = index;
    download->name = file.name;
    download->path = file.path;
    m_task_flags |= FF_DOWNLOAD;
    m_download_seq = SendRequest<Progress>(
        FILE_DOWNLOAD, req,
        [download](json const &resp, Progress &prog, unsigned char const *data) -> int {
            // in work thread, continue recv
            size_t size = resp.value("size", 0);
            prog.size   = resp["offset"];
            prog.total  = resp["total"];
            if (prog.size == 0) {
                download->ofs.open(download->local_path, std::ios::binary);
                if (!download->ofs) {
                    download->error = last_system_error();
                    wxLogWarning("PrinterFileSystem::DownloadNextFile open error: %s\n", wxString::FromUTF8(download->error));
                    return FILE_OPEN_ERR;
                }
            }
            if (download->total && (download->size != prog.size || download->total != prog.total)) {
                wxLogWarning("PrinterFileSystem::DownloadNextFile data error: %d != %d\n", download->size, prog.size);
            }
            // receive data
            download->ofs.write((char const *) data, size);
            if (!download->ofs) {
                download->error = last_system_error();
                wxLogWarning("PrinterFileSystem::DownloadNextFile write error: %s\n", wxString::FromUTF8(download->error));
                return FILE_READ_WRITE_ERR;
            }
            download->boost_md5.process_bytes(data, size);
            prog.size += size;
            download->total = prog.total;
            download->size = prog.size;
            if (prog.size < prog.total) { return 0; }
            download->ofs.close();
            int         result = 0;
            std::string md5    = resp["file_md5"];
            // check size and md5
            if (prog.size == prog.total) {
                boost::uuids::detail::md5::digest_type digest;
                download->boost_md5.get_digest(digest);
                for (int i = 0; i < 4; ++i) digest[i] = boost::endian::endian_reverse(digest[i]);
                std::string str_md5;
                const auto  char_digest = reinterpret_cast<const char *>(&digest[0]);
                boost::algorithm::hex(char_digest, char_digest + sizeof(digest), std::back_inserter(str_md5));
                if (!boost::iequals(str_md5, md5)) {
                    wxLogWarning("PrinterFileSystem::DownloadNextFile checksum error: %s != %s\n", str_md5, md5);
                    boost::system::error_code ec;
                    boost::filesystem::rename(download->local_path, download->local_path + ".tmp", ec);
                    result = FILE_CHECK_ERR;
                }
            } else {
                result = FILE_SIZE_ERR;
            }
            if (result != 0) {
                boost::system::error_code ec;
                boost::filesystem::remove(download->local_path, ec);
            }
            return result;
        },
        [this, download, type = std::make_pair(m_file_type, m_file_storage)](int result, Progress const &data) {
            int progress = data.total ? data.size * 100 / data.total : 0;
            if (result == CONTINUE) {
                if (download->progress == progress)
                    return;
            }
            download->progress = progress;
            if (download->index != size_t(-1)) {
                auto file_index = FindFile(type, download->index, download->path.empty() ? download->name : download->path, !download->path.empty());
                download->index = file_index.second;
                if (download->index != size_t(-1)) {
                    auto &file = file_index.first[download->index];
                    if (result == CONTINUE)
                        ;
                    else if (result == SUCCESS)
                        file.download.reset(), file.local_path = download->local_path;
                    else if (result == ERROR_CANCEL)
                        file.download.reset(), file.flags &= ~FF_DOWNLOAD;
                    else // FAILED
                        file.download.reset();
                    if (&file_index.first == &m_file_list)
                        SendChangedEvent(EVT_DOWNLOAD, download->index, result ? download->error : file.local_path, result);
                }
            }
            if (result != CONTINUE) DownloadNextFile();
        });
}

enum ThumbnailType
{
    OldThumbnail = 0,
    VideoThumbnail = 1,
    ModelMetadata = 2,
    ModelThumbnail = 3,
    FinishThumbnail
};

std::string PrinterFileSystem::StorageCachePath(File const &file, char const *extension) const
{
    if (m_cache_scope.empty() || file.time == 0 || file.size == 0)
        return {};

    std::ostringstream identity;
    identity << STORAGE_CACHE_VERSION << '\n'
             << m_cache_scope << '\n'
             << m_file_storage << '\n'
             << file.path << '\n'
             << file.name << '\n'
             << file.size << '\n'
             << static_cast<long long>(file.time);

    boost::filesystem::path dir = boost::filesystem::path(Slic3r::data_dir()) /
                                  "storage_cache" /
                                  m_cache_scope;
    return (dir / (storage_cache_hash(identity.str()) + extension)).string();
}

PrinterFileSystem::StorageCacheLoad PrinterFileSystem::TryLoadStorageCache(File &file)
{
    if (m_file_type != F_MODEL)
        return StorageCacheLoad::Miss;

    auto json_name = StorageCachePath(file, ".json");
    if (json_name.empty())
        return StorageCacheLoad::Miss;

    boost::filesystem::path json_path(json_name);
    if (!boost::filesystem::exists(json_path)) {
        BOOST_LOG_TRIVIAL(info) << "[StorageCache] miss path=" << file.path;
        return StorageCacheLoad::Miss;
    }

    try {
        boost::filesystem::ifstream stream(json_path);
        if (!stream)
            return StorageCacheLoad::Miss;

        json entry;
        stream >> entry;
        if (entry.value("version", 0) != STORAGE_CACHE_VERSION ||
            entry.value("storage", std::string()) != m_file_storage ||
            entry.value("path", std::string()) != file.path ||
            entry.value("name", std::string()) != file.name ||
            entry.value("size", boost::uint64_t(0)) != file.size ||
            entry.value("time", int64_t(0)) != static_cast<int64_t>(file.time)) {
            BOOST_LOG_TRIVIAL(info) << "[StorageCache] stale path=" << file.path;
            return StorageCacheLoad::Miss;
        }

        auto metadata = entry.at("metadata").get<std::map<std::string, std::string>>();
        bool has_thumbnail = entry.value("has_thumbnail", false);
        if (has_thumbnail) {
            auto png_name = StorageCachePath(file, ".png");
            boost::filesystem::path png_path(png_name);
            if (!boost::filesystem::exists(png_path))
                return StorageCacheLoad::Miss;
            std::string png_utf8 = Slic3r::decode_path(png_path.string().c_str());
            wxImage image;
            if (!image.LoadFile(wxString::FromUTF8(png_utf8.c_str()), wxBITMAP_TYPE_PNG) || !image.IsOk())
                return StorageCacheLoad::Miss;
            file.thumbnail = wxBitmap(image);
        }

        file.metadata = std::move(metadata);
        auto thumbnail_it = file.metadata.find("Thumbnail");
        const bool expects_thumbnail = thumbnail_it != file.metadata.end() && !thumbnail_it->second.empty();

        if (!has_thumbnail && expects_thumbnail) {
            BOOST_LOG_TRIVIAL(info) << "[StorageCache] metadata hit path=" << file.path;
            return StorageCacheLoad::MetadataOnly;
        }

        file.flags |= FF_THUMNAIL;
        BOOST_LOG_TRIVIAL(info) << "[StorageCache] complete hit path=" << file.path
                                << " thumbnail=" << has_thumbnail;
        return StorageCacheLoad::Complete;
    } catch (std::exception const &e) {
        BOOST_LOG_TRIVIAL(warning) << "[StorageCache] read failed path=" << file.path
                                   << " error=" << e.what();
        return StorageCacheLoad::Miss;
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "[StorageCache] read failed path=" << file.path;
        return StorageCacheLoad::Miss;
    }
}

void PrinterFileSystem::SaveStorageCache(File const &file, bool include_thumbnail) const
{
    if (m_file_type != F_MODEL || file.metadata.empty())
        return;

    auto json_name = StorageCachePath(file, ".json");
    if (json_name.empty())
        return;

    bool has_thumbnail = include_thumbnail && file.thumbnail.IsOk();

    try {
        boost::filesystem::path json_path(json_name);
        boost::filesystem::create_directories(json_path.parent_path());

        if (has_thumbnail) {
            boost::filesystem::path png_path(StorageCachePath(file, ".png"));
            std::string png_utf8 = Slic3r::decode_path(png_path.string().c_str());
            wxImage image = file.thumbnail.ConvertToImage();
            if (!image.IsOk() ||
                !image.SaveFile(wxString::FromUTF8(png_utf8.c_str()), wxBITMAP_TYPE_PNG)) {
                BOOST_LOG_TRIVIAL(warning) << "[StorageCache] thumbnail write failed path=" << file.path;
                return;
            }
        }

        json entry;
        entry["version"] = STORAGE_CACHE_VERSION;
        entry["storage"] = m_file_storage;
        entry["path"] = file.path;
        entry["name"] = file.name;
        entry["size"] = file.size;
        entry["time"] = static_cast<int64_t>(file.time);
        entry["metadata"] = file.metadata;
        entry["has_thumbnail"] = has_thumbnail;

        boost::filesystem::ofstream stream(json_path, std::ios::trunc);
        if (!stream)
            return;
        stream << entry.dump();
        stream.close();

        BOOST_LOG_TRIVIAL(info) << "[StorageCache] saved path=" << file.path
                                << " thumbnail=" << has_thumbnail;
    } catch (std::exception const &e) {
        BOOST_LOG_TRIVIAL(warning) << "[StorageCache] write failed path=" << file.path
                                   << " error=" << e.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "[StorageCache] write failed path=" << file.path;
    }
}

void PrinterFileSystem::UpdateFocusThumbnail()
{
    m_task_flags &= ~FF_THUMNAIL;
    if (m_lock_start >= m_file_list.size() || m_lock_start >= m_lock_end)
        return;
    size_t start = m_lock_start;
    size_t end   = std::min(m_lock_end, GetCount());
    std::vector<File> names;
    std::vector<File> paths;
    bool start_with_cached_metadata = false;
    const size_t batch_limit = m_file_type == F_MODEL ? 1 : 2; // Serialize model SUB_FILE chains on A1.
    for (; start < end; ++start) {
        auto &file = GetFile(start);
        if ((file.flags & FF_THUMNAIL) == 0) {
            if (m_file_type == F_MODEL) {
                auto &mutable_file = const_cast<File &>(file);
                auto cache = TryLoadStorageCache(mutable_file);
                if (cache == StorageCacheLoad::Complete) {
                    SendChangedEvent(EVT_THUMBNAIL, start, file.name);
                    continue;
                }
                if (cache == StorageCacheLoad::MetadataOnly && !file.path.empty()) {
                    File cached_file = mutable_file;
                    // ModelThumbnail normally follows a successful ModelMetadata response,
                    // where the temporary File is marked complete before entering the next
                    // phase. Preserve the same state so retry bookkeeping does not discard
                    // the cached metadata.
                    cached_file.flags |= FF_THUMNAIL;
                    paths.push_back(std::move(cached_file));
                    start_with_cached_metadata = true;
                } else {
                    mutable_file.metadata.emplace("Time", "...");
                    mutable_file.metadata.emplace("Weight", "...");
                }
            }
            if (!start_with_cached_metadata) {
                if (file.path.empty())
                    names.push_back({file.name, ""});
                else
                    paths.push_back({file.name, file.path});
            }
            if (names.size() >= batch_limit || paths.size() >= batch_limit)
                break;
            if ((file.flags & FF_THUMNAIL_RETRY) != 0) {
                const_cast<File&>(file).flags &= ~FF_THUMNAIL_RETRY;
                break;
            }
        }
    }
    if (names.empty() && paths.empty())
        return;
    m_task_flags |= FF_THUMNAIL;
    const auto &batch = paths.empty() ? names : paths;
    if (m_file_type == F_MODEL && batch.size() == 1) {
        BOOST_LOG_TRIVIAL(info) << "[StorageTrace] serial model chain"
                                << " storage=" << m_file_storage
                                << " file=" << batch.front().name
                                << " path=" << batch.front().path
                                << " cached_metadata=" << start_with_cached_metadata;
    }
    UpdateFocusThumbnail2(std::make_shared<std::vector<File>>(batch),
        paths.empty() ? OldThumbnail :
        m_file_type == F_MODEL ? (start_with_cached_metadata ? ModelThumbnail : ModelMetadata) :
        VideoThumbnail);
}

bool PrinterFileSystem::ParseThumbnail(File &file)
{
    std::istringstream iss(file.local_path, std::ios::binary);
    return ParseThumbnail(file, iss);
}

static std::string durationString(long duration)
{
    static boost::regex rx("^0d(0h)?");
    auto time = boost::format("%1%d%2%h%3%m") % (duration / 86400) % ((duration % 86400) / 3600) % ((duration % 3600) / 60);
    return boost::regex_replace(time.str(), rx, "");
}

bool PrinterFileSystem::ParseThumbnail(File &file, std::istream &is)
{
    Slic3r::DynamicPrintConfig config;
    Slic3r::Model              model;
    Slic3r::PlateDataPtrs      plate_data_list;
    Slic3r::Semver file_version;
    if (!Slic3r::load_gcode_3mf_from_stream(is, &config, &model, &plate_data_list, &file_version))
        return false;
    float time      = 0.f;
    float weight    = 0.f;
    for (auto &plate : plate_data_list) {
        time += atof(plate->gcode_prediction.c_str());
        weight += atof(plate->gcode_weight.c_str());
        if (!plate->gcode_file.empty() && !plate->thumbnail_file.empty())
            file.metadata.emplace("plate_thumbnail_" + std::to_string(plate->plate_index), plate->thumbnail_file);
    }
    file.metadata.emplace("Title", model.model_info->model_name);
    file.metadata.emplace("Time", durationString(round(time)));
    file.metadata.emplace("Weight", std::to_string(int(round(weight))) + 'g');
    auto thumbnail = model.model_info->metadata_items["Thumbnail"];
    if (thumbnail.empty() && !plate_data_list.empty()) {
        thumbnail = plate_data_list.front()->thumbnail_file;
    }
    file.metadata.emplace("Thumbnail", thumbnail);
    return true;
}

void PrinterFileSystem::UpdateFocusThumbnail2(std::shared_ptr<std::vector<File>> files, int type, int retry)
{
    json req;
    json arr;
    if (!m_file_storage.empty())
        req["storage"] = m_file_storage;
    if (type == OldThumbnail) {
        for (auto &file : *files) arr.push_back(file.name);
        req["files"] = arr;
    } else {
        if (type == VideoThumbnail) {
            for (auto &file : *files) arr.push_back(file.path + "#thumbnail");
        } else if (type == ModelMetadata) {
            for (auto &file : *files) {
                arr.push_back(file.path + "#_rels/.rels");
                arr.push_back(file.path + "#3D/3dmodel.model");
                arr.push_back(file.path + "#Metadata/model_settings.config");
                arr.push_back(file.path + "#Metadata/slice_info.config");
                arr.push_back(file.path + "#Metadata/project_settings.config");
            }
            req["zip"] = true;
        } else { // ModelThumbnail, FinishThumbnail
            std::vector<std::string> fails;
            for (auto &file : *files) {
                if ((file.flags & FF_THUMNAIL) == 0) {
                    fails.push_back(file.path);
                    file.flags |= FF_THUMNAIL;
                }
            }
            for (auto &path : fails) {
                auto iter = std::find_if(m_file_list.begin(), m_file_list.end(), [&path](auto &f) { return f.path == path; });
                if (iter != m_file_list.end()) {
                    if (type == ModelThumbnail) {
                        auto source = std::find_if(files->begin(), files->end(),
                                                   [&path](auto &f) { return f.path == path; });
                        if (source == files->end() || source->metadata.empty())
                            iter->metadata.clear();
                    }
                    iter->flags |= fails.size() == 1 ? FF_THUMNAIL : FF_THUMNAIL_RETRY;
                }
            }
            if (type == ModelThumbnail) {
                for (auto &file : *files) {
                    auto thumbnail = file.metadata["Thumbnail"];
                    if (!thumbnail.empty()) {
                        arr.push_back(file.path + "#" + thumbnail);
                        file.flags &= ~FF_THUMNAIL;
                        file.local_path.clear();
                    }
                }
            }
            if (arr.empty()) {
                UpdateFocusThumbnail();
                return;
            }
        }
        req["paths"] = arr;
    }
    BOOST_LOG_TRIVIAL(info) << "[StorageTrace] SUB_FILE request type=" << type
                            << " retry=" << retry
                            << " storage=" << m_file_storage
                            << " req=" << req.dump();

    SendRequest<File>(
        SUB_FILE, req, [type, files](json const &resp, File &file, unsigned char const *data) -> int {
            BOOST_LOG_TRIVIAL(info) << "[StorageTrace] SUB_FILE response type=" << type
                                    << " path=" << resp.value("path", "")
                                    << " thumbnail=" << resp.value("thumbnail", "")
                                    << " size=" << resp.value("size", 0)
                                    << " continue=" << resp.value("continue", false);
            // in work thread, continue recv
            // receive data
            wxString        mimetype  = resp.value("mimetype", "");
            std::string     thumbnail = resp.value("thumbnail", "");
            std::string     path      = resp.value("path", "");
            boost::uint32_t size      = resp.value("size", 0);
            bool            cont      = resp.value("continue", false);
            if (size == 0) {
                file.name = thumbnail;
                file.path = path;
                return FILE_SIZE_ERR;
            }
            auto n = type == ModelMetadata ? std::string::npos : path.find_last_of('#'); // ModelMetadata is zipped without subpath
            auto path2 = n == std::string::npos ? path : path.substr(0, n);
            auto subpath = n == std::string::npos ? path : path.substr(n + 1);
            auto iter = std::find_if(files->begin(), files->end(), [&path2](auto &f) { return f.path == path2; });
            if (cont) {
                if (iter != files->end())
                    iter->local_path += std::string((char *) data, size);
                return 0;
            }
            if (type == ModelMetadata) {
                if (iter != files->end())
                    file.local_path = iter->local_path + std::string((char *) data, size);
                else
                    file.local_path = std::string((char *) data, size);
                bool parsed = ParseThumbnail(file);
                BOOST_LOG_TRIVIAL(info) << "[StorageTrace] ModelMetadata parsed"
                                           << " path=" << path
                                           << " bytes=" << file.local_path.size()
                                           << " ok=" << parsed
                                           << " thumbnail=" << file.metadata["Thumbnail"];
            } else {
                if (mimetype.empty()) {
                    if (subpath.empty()) subpath = thumbnail;
                    auto n = subpath.find_last_of('.');
                    if (n != std::string::npos)
                        mimetype = "image/" + subpath.substr(n + 1);
                    else if (subpath == "thumbnail")
                        mimetype = "image/jpeg"; // default jpg
                }
                if (iter != files->end() && !iter->local_path.empty()) {
                    iter->local_path += std::string((char *) data, size);
                    data = reinterpret_cast<unsigned char const *>(iter->local_path.c_str());
                    size = iter->local_path.size();
                }
                wxMemoryInputStream mis(data, size);
                mimetype.Replace("jpg", "jpeg");
                file.thumbnail = wxImage(mis, mimetype);
                if (!file.thumbnail.IsOk()) {
                    BOOST_LOG_TRIVIAL(info) << "PrinterFileSystem: parse thumbnail failed" << size << mimetype;
                }
            }
            file.name = thumbnail;
            file.path = path;
            return 0;
        },
        [this, files, type, retry](int result, File const &file) {
            BOOST_LOG_TRIVIAL(info) << "[StorageTrace] SUB_FILE callback type=" << type
                                    << " result=" << result
                                    << " file=" << file.name
                                    << " path=" << file.path;
            auto n    = file.name.find_last_of('.');
            auto name  = n == std::string::npos ? file.name : file.name.substr(0, n) + ".mp4";
            n          = (type == ModelMetadata) ? std::string::npos : file.path.find_last_of('#');
            auto path = n == std::string::npos ? file.path : file.path.substr(0, n);
            auto iter = path.empty() ? std::find_if(m_file_list.begin(), m_file_list.end(), [&name](auto &f) { return f.name == name; }) :
                                       std::find_if(m_file_list.begin(), m_file_list.end(), [&path](auto &f) { return f.path == path; });
            auto iter2 = path.empty() ? std::find_if(files->begin(), files->end(), [&name](auto &f) { return f.name == name; }) :
                                        std::find_if(files->begin(), files->end(), [&path](auto &f) { return f.path == path; });
            if (iter != m_file_list.end()) {
                if (type == ModelMetadata) {
                    iter->metadata = file.metadata;
                    // Persist phase 1 immediately. If the following thumbnail request
                    // fails or drops the tunnel, the next session can resume directly
                    // with ModelThumbnail instead of fetching the five 3MF metadata
                    // members again.
                    SaveStorageCache(*iter, false);
                    auto thumbnail = iter->metadata["Thumbnail"];
                    if (thumbnail.empty()) {
                        iter->flags |= FF_THUMNAIL; // DOTO: retry on fail
                    }
                    int index       = iter - m_file_list.begin();
                    SendChangedEvent(EVT_THUMBNAIL, index, file.name);
                    if (iter2 != files->end())
                        iter2->metadata = file.metadata;
                } else {
                    iter->flags |= FF_THUMNAIL; // DOTO: retry on fail
                    if (file.thumbnail.IsOk()) {
                        iter->thumbnail = file.thumbnail;
                        File cache_file = *iter;
                        if (iter2 != files->end())
                            cache_file.metadata = iter2->metadata;
                        SaveStorageCache(cache_file, true);
                        int index       = iter - m_file_list.begin();
                        SendChangedEvent(EVT_THUMBNAIL, index, file.name);
                    }
                }
            }
            if (result == CONTINUE)
                return;

            if (result != SUCCESS) {
                BOOST_LOG_TRIVIAL(warning) << "[StorageTrace] thumbnail request error"
                                           << " type=" << type
                                           << " result=" << result
                                           << " retry=" << retry
                                           << " storage=" << m_file_storage
                                           << " file=" << file.name
                                           << " path=" << file.path;

                // A1 may temporarily return FILE_READ_WRITE_ERR while Storage thumbnails
                // are being fetched quickly. Retry the same request with bounded backoff
                // instead of immediately advancing and collapsing the file tunnel.
                const bool retryable = result == FILE_READ_WRITE_ERR ||
                                       result == ERROR_RES_BUSY ||
                                       result == ERROR_TIME_OUT;
                if (retryable && retry < 3) {
                    if (type == ModelThumbnail || type == FinishThumbnail) {
                        for (auto &f : *files) {
                            f.flags &= ~FF_THUMNAIL;
                            auto it = std::find_if(m_file_list.begin(), m_file_list.end(),
                                                   [&f](auto &entry) { return entry.path == f.path; });
                            if (it != m_file_list.end())
                                it->flags &= ~FF_THUMNAIL;
                        }
                    }
                    const int delay_ms = 500 << retry; // 500, 1000, 2000 ms
                    BOOST_LOG_TRIVIAL(info) << "[StorageTrace] retrying thumbnail request"
                                            << " type=" << type
                                            << " retry=" << (retry + 1)
                                            << " delay_ms=" << delay_ms;
                    ScheduleThumbnailUpdate(files, type, retry + 1, delay_ms);
                    return;
                }

                if (result == ERROR_PIPE) {
                    // Keep the failed model unresolved. Reconnect performs LIST_INFO again,
                    // and the partial metadata cache lets the next attempt resume at the
                    // thumbnail phase instead of repeating the five-file metadata fetch.
                    for (auto &f : *files) {
                        f.flags &= ~(FF_THUMNAIL | FF_THUMNAIL_RETRY);
                        auto it = std::find_if(m_file_list.begin(), m_file_list.end(),
                                               [&f](auto &entry) { return entry.path == f.path; });
                        if (it != m_file_list.end())
                            it->flags &= ~(FF_THUMNAIL | FF_THUMNAIL_RETRY);
                    }
                    BOOST_LOG_TRIVIAL(warning) << "[StorageTrace] pipe lost; keeping thumbnail unresolved";
                    return;
                }

                // A permanently unreadable thumbnail must not make the whole Storage view
                // unusable. Mark only this file as handled and continue with the next one.
                for (auto &f : *files) {
                    f.flags |= FF_THUMNAIL;
                    auto it = std::find_if(m_file_list.begin(), m_file_list.end(),
                                           [&f](auto &entry) { return entry.path == f.path; });
                    if (it != m_file_list.end())
                        it->flags |= FF_THUMNAIL;
                }

                ScheduleThumbnailUpdate(files, FinishThumbnail, 0, 250);
                return;
            }

            if (iter2 != files->end())
                iter2->flags |= FF_THUMNAIL; // have received successful response

            if (type == ModelMetadata) {
                UpdateFocusThumbnail2(files, ModelThumbnail, 0);
            } else {
                // Give the printer a small breather between completed model chains.
                ScheduleThumbnailUpdate(files, FinishThumbnail, 0, 150);
            }
        });
}

void PrinterFileSystem::ScheduleThumbnailUpdate(std::shared_ptr<std::vector<File>> files, int type, int retry, int delay_ms)
{
    auto weak = weak_from_this();
    auto storage = m_file_storage;
    boost::thread([weak, files, type, retry, delay_ms, storage] {
        boost::this_thread::sleep(boost::posix_time::milliseconds(delay_ms));
        auto self = weak.lock();
        if (!self)
            return;
        self->PostCallback([self, files, type, retry, storage] {
            if (self->m_stopped || self->m_file_type != F_MODEL || self->m_file_storage != storage)
                return;
            self->UpdateFocusThumbnail2(files, type, retry);
        });
    }).detach();
}

std::pair<PrinterFileSystem::FileList &, size_t> PrinterFileSystem::FindFile(std::pair<FileType, std::string> type, size_t index, std::string const &name, bool by_path)
{
    FileList & file_list = type == std::make_pair(m_file_type, m_file_storage) ?
                               m_file_list :
                               m_file_list_cache[type];
    if (index >= file_list.size() || (by_path ? file_list[index].path : file_list[index].name) != name) {
        auto iter = std::find_if(m_file_list.begin(), file_list.end(),
                [name, by_path](File &f) { return (by_path ? f.path : f.name) == name; });
        if (iter == m_file_list.end()) return {file_list, -1};
        index = std::distance(m_file_list.begin(), iter);
    }
    return {file_list, index};
}

void PrinterFileSystem::FileRemoved(std::pair<FileType, std::string> type, size_t index, std::string const &name, bool by_path)
{
    auto file_index = FindFile(type, index, name, by_path);
    if (file_index.second == size_t(-1))
        return;
    if (&file_index.first == &m_file_list) {
        auto removeFromGroup = [](std::vector<size_t> &group, size_t index, size_t total) {
            for (auto iter = group.begin(); iter != group.end(); ++iter) {
                size_t index2 = -1;
                if (*iter < index) continue;
                if (*iter == index) {
                    auto iter2 = iter + 1;
                    if (index + 1 == (iter2 == group.end() ? total : *iter2)) {
                        index2 = std::distance(group.begin(), iter);
                    }
                    ++iter;
                }
                for (; iter != group.end(); ++iter) {
                    --*iter;
                }
                return index2;
            }
            return size_t(-1);
        };
        size_t index2 = removeFromGroup(m_group_month, index, m_file_list.size());
        if (index2 < m_group_month.size()) {
            int index3 = removeFromGroup(m_group_year, index2, m_group_month.size());
            if (index3 < m_group_year.size()) {
                m_group_year.erase(m_group_year.begin() + index3);
                if (m_group_mode == G_YEAR)
                    m_group_flags.erase(m_group_flags.begin() + index3);
            }
            m_group_month.erase(m_group_month.begin() + index2);
            if (m_group_mode == G_MONTH)
                m_group_flags.erase(m_group_flags.begin() + index2);
        }
    }
    file_index.first.erase(file_index.first.begin() + index);
}

struct CallbackEvent : wxCommandEvent
{
    CallbackEvent(std::function<void(void)> const &callback, boost::weak_ptr<PrinterFileSystem> owner) : wxCommandEvent(EVT_FILE_CALLBACK), callback(callback), owner(owner) {}
    ~CallbackEvent(){ if (!owner.expired()) callback(); }
    std::function<void(void)> const callback;
    boost::weak_ptr<PrinterFileSystem> owner;
};

void PrinterFileSystem::PostCallback(std::function<void(void)> const& callback)
{
    wxCommandEvent *e = new CallbackEvent(callback, boost::weak_ptr(shared_from_this()));
    wxQueueEvent(this, e);
}

void PrinterFileSystem::SendChangedEvent(wxEventType type, size_t index, std::string const &str, long extra)
{
    wxCommandEvent event(type);
    event.SetEventObject(this);
    event.SetInt(index);
    if (!str.empty())
        event.SetString(wxString::FromUTF8(str.c_str()));
    else if (auto iter = error_messages.find(extra); iter != error_messages.end())
        event.SetString(_L(iter->second.c_str()));
    else if (extra > CONTINUE && extra != ERROR_CANCEL)
        event.SetString(wxString::Format(_L("Error code: %d"), int(extra)));
    event.SetExtraLong(extra);
    if (wxThread::IsMain())
        ProcessEventLocally(event);
    else
        wxPostEvent(this, event);
}

void PrinterFileSystem::DumpLog(void * thiz, int, tchar const *msg)
{

#if !BBL_RELEASE_TO_PUBLIC
    BOOST_LOG_TRIVIAL(info) << "PrinterFileSystem: " << wxString(msg).ToUTF8().data();
#endif

    static_cast<PrinterFileSystem*>(thiz)->Bambu_FreeLogMsg(msg);
}

boost::uint32_t PrinterFileSystem::RequestMediaAbility(int api_version)
{
    json req;
    req["peer"] = "studio";
    req["api_version"] = api_version;

    return SendRequest<MediaAbilityList>(
        REQUEST_MEDIA_ABILITY, req, [](const json &resp, MediaAbilityList &list, auto) -> int {
            json abliity_list = resp["storage"];
            list              = abliity_list.get<MediaAbilityList>();
            return 0;
        },
        [this](int result, MediaAbilityList list){
            if (result != 0) {
                m_last_error = result;
                m_media_ability_list.clear();
                SendChangedEvent(EVT_MEDIA_ABILITY_CHANGED, RequestMediaAbilityStatus::S_FAILED, "", m_last_error);
                return result;
            }

            m_media_ability_list.swap(list);
            SendChangedEvent(EVT_MEDIA_ABILITY_CHANGED, RequestMediaAbilityStatus::S_SUCCESS);
            return 0;
        });
}

void PrinterFileSystem::RequestUploadFile()
{
    if (m_use_ftps) {
        RequestFtpsUpload();
        return;
    }

    if (!m_upload_file) {
        return;
    }

    json req;
    req["type"]    = "model";
    req["storage"] = m_upload_file->select_storage;
    req["path"]    = m_upload_file->name;

    m_upload_file->upload = std::make_unique<Upload>();
    boost::filesystem::path   path = boost::filesystem::path(m_upload_file->path);
    boost::system::error_code ec;
    boost::uint32_t           file_size = boost::filesystem::file_size(path, ec);

    req["total"] = file_size;
    m_upload_file->size          = file_size;
    m_upload_file->upload->total = file_size;

    m_upload_seq = SendRequest(
        FILE_UPLOAD, req,
        [this](int result, const json& resp, auto) -> int{
            if (result != SUCCESS && result != CONTINUE && result != FILE_EXIST) {
                std::string error_msg = "";
                if (result == ERROR_CANCEL) {
                    error_msg = _L("User cancels task.").ToStdString();
                } else if (result == FILE_READ_WRITE_ERR || result == FILE_OPEN_ERR) {
                    error_msg = _L("Failed to read file, please try again.").ToStdString();
                }
                wxLogWarning("PrinterFileSystem::UploadFile error: %d\n", result);
                SendChangedEvent(EVT_UPLOAD_CHANGED, FF_UPLOADCANCEL, error_msg, result);
            } else if (result == SUCCESS) {
                SendChangedEvent(EVT_UPLOADING, 100);
                SendChangedEvent(EVT_UPLOAD_CHANGED, FF_UPLOADDONE);
            } else if (result == CONTINUE || result == FILE_EXIST) {
                if (m_upload_file) {
                    m_upload_file->chunk_size   = resp["chunk_size"];
                    m_upload_file->upload->size = resp["offset"];
                    m_upload_file->flags |= FF_UPLOADING;
                }

                {
                    boost::unique_lock l(m_mutex);
                    auto cb = [this, upload_file = m_upload_file, seq = m_upload_seq](std::string &msg) -> int {
                        return UploadFileTask(upload_file, seq, msg);
                    };
                    m_produce_message_cb_map[m_upload_seq] = cb;
                }

                return CONTINUE;
            }

            // reset m_upload_file
            if (m_upload_file) {
                if (m_upload_file->upload->ifs.is_open()) {
                    m_upload_file->upload->ifs.close();
                }
                m_upload_file.reset();
            }
            return result;
        });
}

int PrinterFileSystem::UploadFileTask(std::shared_ptr<UploadFile> upload_file, boost::uint64_t seq, std::string &msg)
{
    if (!upload_file)
        return FILE_OPEN_ERR;

    if (!(upload_file->flags & FF_UPLOADING))
        return FILE_OPEN_ERR;

    auto &upload = upload_file->upload;
    if (!upload->ifs.is_open()) {
        upload->ifs.open(upload_file->path, std::ios::binary);
        if (!upload_file->upload->ifs) {
            wxLogWarning("PrinterFileSystem::UploadFile open error: %s\n", wxString::FromUTF8(upload_file->path));
            return FILE_OPEN_ERR;
        }
        MD5_Init(&upload->ctx);
    }

    const boost::uint32_t buffer_size = upload_file->chunk_size * 1024;
    char *buffer = new char[buffer_size];

    upload->ifs.seekg(upload->size, std::ios::beg);
    upload->ifs.read(buffer, buffer_size);
    boost::int32_t read_size = upload->ifs.gcount();

    if (read_size <= 0) {
        wxLogWarning("PrinterFileSystem::Upload read error.\n");
        upload->ifs.close();

        if (buffer) {
            delete[] buffer;
            buffer = nullptr;
        }
        return FILE_READ_WRITE_ERR;
    }

    json req;
    req["frag_id"] = upload->frag_id;
    req["offset"]  = upload->size;
    req["size"]    = read_size;

    MD5_Update(&upload->ctx, buffer, read_size);
    upload->size += read_size;
    if (upload->size == upload->total) {
        unsigned char digest[16];
        MD5_Final(digest, &upload->ctx);
        char md5_str[33];
        for (int j = 0; j < 16; j++) { sprintf(&md5_str[j * 2], "%02X", (unsigned int) digest[j]); }
        std::string md5_out = std::string(md5_str);
        std::transform(md5_out.begin(), md5_out.end(), md5_out.begin(), ::tolower);

        req["file_md5"]     = md5_out;
        // OutputDebugStringA(md5_out.c_str());
        // OutputDebugStringA("\n");
    }

    if (m_upload_file && m_upload_file->flags & FF_UPLOADING) {
        upload->frag_id++;
        upload->progress = upload->size * 100 / upload->total;
        int progress     = upload->progress == 100 ? 99 : upload->progress;
        SendChangedEvent(EVT_UPLOADING, progress);
    }

    json root;

    root["cmdtype"] = FILE_UPLOAD;
    root["sequence"] = seq;
    root["req"]      = req;

    std::ostringstream oss;
    oss << root;
    oss << "\n\n";
    oss << std::string(buffer, read_size);
    msg = oss.str();

    if (buffer) {
        delete[] buffer;
        buffer = nullptr;
    }

    if (upload->size == upload->total) {
        upload->ifs.close();
        return SUCCESS;
    }

    return CONTINUE;
}

PrinterFileSystem::MediaAbilityList PrinterFileSystem::GetMediaAbilityList() const
{
    return m_media_ability_list;
}

void PrinterFileSystem::CancelUploadTask(bool send_cancel_req)
{
    if (!m_upload_file)
        return;

    if (m_use_ftps) {
        CancelRequests2({m_upload_seq});
        return;
    }

    {
        boost::unique_lock l(m_mutex);
        if (m_produce_message_cb_map.find(m_upload_seq) != m_produce_message_cb_map.end())
            m_produce_message_cb_map.erase(m_upload_seq);
        if (m_upload_file->upload->ifs.is_open()) {
            m_upload_file->upload->ifs.close();
        }
        m_upload_file.reset();
    }

    if (send_cancel_req) {
        CancelRequest(m_upload_seq);
    } else {
        CancelRequests2({m_upload_seq});
    }
}

bool PrinterFileSystem::EnsureFtpsStorage(std::string &prefix, std::string &label, std::string &error)
{
    std::string host;
    std::string user;
    std::string password;
    {
        boost::unique_lock lock(m_ftps_mutex);
        if (!m_ftps_storage_label.empty()) {
            prefix = m_ftps_prefix;
            label = m_ftps_storage_label;
            error.clear();
            return true;
        }
        host = m_ftps_host;
        user = m_ftps_user;
        password = m_ftps_password;
    }

    BambuFtps::Client client(host, user, password);
    if (!client.ready()) {
        error = "FTPS endpoint is incomplete";
        return false;
    }

    struct Candidate {
        char const *path;
        char const *label;
    };
    static constexpr Candidate candidates[] = {
        {"/sdcard", "sdcard"},
        {"/usb", "udisk"},
        {"/", "udisk"},
    };

    std::string last_error;
    for (auto const &candidate : candidates) {
        std::vector<BambuFtps::Entry> entries;
        std::string current_error = client.list(candidate.path, entries);
        if (current_error.empty()) {
            prefix = candidate.path;
            label = candidate.label;
            {
                boost::unique_lock lock(m_ftps_mutex);
                m_ftps_prefix = prefix;
                m_ftps_storage_label = label;
            }
            BOOST_LOG_TRIVIAL(info) << "[StorageFTPS] storage root=" << prefix
                                    << " label=" << label;
            error.clear();
            return true;
        }
        last_error = current_error;
    }

    error = last_error.empty() ? "No accessible FTPS storage root" : last_error;
    BOOST_LOG_TRIVIAL(warning) << "[StorageFTPS] storage probe failed: " << error;
    return false;
}

bool PrinterFileSystem::IsFtpsCancelled(boost::uint32_t seq)
{
    boost::unique_lock lock(m_ftps_mutex);
    return m_ftps_cancelled.find(seq) != m_ftps_cancelled.end();
}

void PrinterFileSystem::FinishFtpsRequest(boost::uint32_t seq)
{
    boost::unique_lock lock(m_ftps_mutex);
    m_ftps_active.erase(seq);
    m_ftps_cancelled.erase(seq);
}

boost::uint32_t PrinterFileSystem::SendFtpsRequest(int type, json const &req,
                                                   callback_t2 const &callback,
                                                   const std::string &param)
{
    const boost::uint32_t seq = m_ftps_sequence.fetch_add(1);
    {
        boost::unique_lock lock(m_ftps_mutex);
        m_ftps_active.insert(seq);
    }

    boost::thread worker([w = weak_from_this(), seq, type, req, callback, param] {
        if (auto self = w.lock())
            self->DispatchFtpsRequest(seq, type, req, callback, param);
    });
    worker.detach();
    return seq;
}

void PrinterFileSystem::DispatchFtpsRequest(boost::uint32_t seq, int type,
                                            json const &req,
                                            callback_t2 const &callback,
                                            const std::string &param)
{
    (void)param;

    auto finish = [this, seq, &callback](int result, json const &resp,
                                         unsigned char const *data) {
        if (callback)
            callback(result, resp, data);
        FinishFtpsRequest(seq);
    };

    if (IsFtpsCancelled(seq)) {
        finish(ERROR_CANCEL, json::object(), nullptr);
        return;
    }

    std::string host;
    std::string user;
    std::string password;
    {
        boost::unique_lock lock(m_ftps_mutex);
        host = m_ftps_host;
        user = m_ftps_user;
        password = m_ftps_password;
    }

    BambuFtps::Client client(host, user, password);
    std::string prefix;
    std::string storage_label;
    std::string error;
    if (!EnsureFtpsStorage(prefix, storage_label, error)) {
        BOOST_LOG_TRIVIAL(warning) << "[StorageFTPS] request " << type
                                   << " failed before dispatch: " << error;
        finish(STORAGE_UNAVAILABLE, json::object(), nullptr);
        return;
    }

    if (type == REQUEST_MEDIA_ABILITY) {
        json resp;
        resp["storage"] = json::array({storage_label});
        finish(SUCCESS, resp, nullptr);
        return;
    }

    if (type == LIST_INFO) {
        const std::string requested_storage = req.value("storage", std::string());
        if (requested_storage == "internal" || requested_storage == "emmc") {
            json resp;
            resp["file_lists"] = json::array();
            finish(SUCCESS, resp, nullptr);
            return;
        }

        const std::string file_type = req.value("type", std::string("model"));
        const std::string directory = ftps_subtree(prefix, file_type);
        std::vector<BambuFtps::Entry> entries;
        error = client.list(directory, entries);
        if (!error.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "[StorageFTPS] LIST " << directory
                                       << " failed: " << error;
            finish(STORAGE_UNAVAILABLE, json::object(), nullptr);
            return;
        }

        json files = json::array();
        for (auto const &entry : entries) {
            if (entry.is_dir || !ftps_keep_file(file_type, entry.name))
                continue;
            json file;
            file["name"] = entry.name;
            file["path"] = ftps_join_path(directory, entry.name);
            file["size"] = entry.size;
            file["time"] = entry.mtime;
            files.push_back(std::move(file));
        }

        json resp;
        resp["file_lists"] = std::move(files);
        BOOST_LOG_TRIVIAL(info) << "[StorageFTPS] LIST_INFO type=" << file_type
                                << " path=" << directory
                                << " files=" << resp["file_lists"].size();
        finish(SUCCESS, resp, nullptr);
        return;
    }

    if (type == SUB_FILE) {
        if (!req.contains("paths") || !req["paths"].is_array() || req["paths"].empty()) {
            finish(FILE_TYPE_ERR, json::object(), nullptr);
            return;
        }

        const std::string requested = req["paths"].front().get<std::string>();
        const std::string archive_path = ftps_base_path(requested);
        const bool zip_request = req.value("zip", false);

        std::shared_ptr<std::string> archive;
        {
            boost::unique_lock lock(m_ftps_mutex);
            auto iter = m_ftps_archive_cache.find(archive_path);
            if (iter != m_ftps_archive_cache.end())
                archive = iter->second;
        }

        if (!archive) {
            archive = std::make_shared<std::string>();
            error = client.retrieve(
                archive_path,
                [archive](void const *data, size_t size) {
                    archive->append(static_cast<char const *>(data), size);
                    return true;
                },
                [this, seq](std::uint64_t, std::uint64_t) {
                    return !IsFtpsCancelled(seq);
                });
            if (!error.empty()) {
                const int result = IsFtpsCancelled(seq) || error == "cancelled"
                    ? ERROR_CANCEL : FILE_READ_WRITE_ERR;
                BOOST_LOG_TRIVIAL(warning) << "[StorageFTPS] RETR " << archive_path
                                           << " failed: " << error;
                finish(result, json::object(), nullptr);
                return;
            }
        }

        if (zip_request) {
            // The native :6000 service synthesizes a partial zip containing the
            // requested members. Returning the complete 3MF is equivalent for
            // Orca's parser and avoids five separate FTPS downloads.
            if (req["paths"].size() == 5) {
                boost::unique_lock lock(m_ftps_mutex);
                m_ftps_archive_cache[archive_path] = archive;
            }

            json resp;
            resp["size"] = archive->size();
            resp["path"] = archive_path;
            resp["thumbnail"] = boost::filesystem::path(archive_path).filename().string();
            resp["continue"] = false;
            finish(SUCCESS, resp,
                   reinterpret_cast<unsigned char const *>(archive->data()));
            return;
        }

        const std::string member = ftps_sub_path(requested);
        if (member.empty()) {
            finish(FILE_TYPE_ERR, json::object(), nullptr);
            return;
        }

        std::string extracted;
        if (!ftps_extract_zip_entry(*archive, member, extracted)) {
            BOOST_LOG_TRIVIAL(warning) << "[StorageFTPS] 3MF member not found: "
                                       << archive_path << "#" << member;
            finish(FILE_NO_EXIST, json::object(), nullptr);
            return;
        }

        {
            boost::unique_lock lock(m_ftps_mutex);
            m_ftps_archive_cache.erase(archive_path);
        }

        json resp;
        resp["size"] = extracted.size();
        resp["path"] = requested;
        resp["thumbnail"] = boost::filesystem::path(member).filename().string();
        resp["continue"] = false;
        const std::string ext = boost::filesystem::path(member).extension().string();
        if (boost::iequals(ext, ".png"))
            resp["mimetype"] = "image/png";
        else if (boost::iequals(ext, ".jpg") || boost::iequals(ext, ".jpeg"))
            resp["mimetype"] = "image/jpeg";

        finish(SUCCESS, resp,
               reinterpret_cast<unsigned char const *>(extracted.data()));
        return;
    }

    if (type == FILE_DOWNLOAD) {
        std::string remote = req.value("path", std::string());
        if (remote.empty()) {
            const std::string name = req.value("file", std::string());
            const char *types[] = {"timelapse", "video", "model"};
            const int ft = std::max(0, std::min(static_cast<int>(m_file_type), 2));
            remote = ftps_join_path(ftps_subtree(prefix, types[ft]), name);
        }

        std::uint64_t total = 0;
        error = client.size(remote, total);
        if (!error.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "[StorageFTPS] SIZE " << remote
                                       << " failed: " << error;
            finish(FILE_SIZE_ERR, json::object(), nullptr);
            return;
        }

        boost::uuids::detail::md5 md5;
        std::uint64_t offset = 0;
        bool translator_failed = false;
        error = client.retrieve(
            remote,
            [this, seq, &callback, &md5, &offset, total, &translator_failed](void const *data, size_t size) {
                if (IsFtpsCancelled(seq))
                    return false;
                json resp;
                resp["offset"] = offset;
                resp["total"] = total;
                resp["size"] = size;
                resp["file_md5"] = "";
                const int cb_result = callback
                    ? callback(CONTINUE, resp, reinterpret_cast<unsigned char const *>(data))
                    : CONTINUE;
                if (cb_result != CONTINUE) {
                    translator_failed = true;
                    return false;
                }
                md5.process_bytes(data, size);
                offset += size;
                return true;
            },
            [this, seq](std::uint64_t, std::uint64_t) {
                return !IsFtpsCancelled(seq);
            });

        if (translator_failed) {
            FinishFtpsRequest(seq);
            return;
        }
        if (!error.empty()) {
            const int result = IsFtpsCancelled(seq) || error == "cancelled"
                ? ERROR_CANCEL : FILE_READ_WRITE_ERR;
            finish(result, json::object(), nullptr);
            return;
        }

        json resp;
        resp["offset"] = offset;
        resp["total"] = total;
        resp["size"] = 0;
        resp["file_md5"] = storage_md5_hex(md5);
        static unsigned char dummy = 0;
        finish(SUCCESS, resp, &dummy);
        return;
    }

    if (type == FILE_DEL) {
        std::vector<std::string> paths;
        if (req.contains("paths") && req["paths"].is_array()) {
            for (auto const &item : req["paths"])
                paths.push_back(item.get<std::string>());
        } else if (req.contains("delete") && req["delete"].is_array()) {
            const char *types[] = {"timelapse", "video", "model"};
            const int ft = std::max(0, std::min(static_cast<int>(m_file_type), 2));
            const std::string directory = ftps_subtree(prefix, types[ft]);
            for (auto const &item : req["delete"])
                paths.push_back(ftps_join_path(directory, item.get<std::string>()));
        }

        for (auto const &path : paths) {
            if (IsFtpsCancelled(seq)) {
                finish(ERROR_CANCEL, json::object(), nullptr);
                return;
            }
            error = client.remove(path);
            if (!error.empty()) {
                BOOST_LOG_TRIVIAL(warning) << "[StorageFTPS] DELE " << path
                                           << " failed: " << error;
                finish(FILE_READ_WRITE_ERR, json::object(), nullptr);
                return;
            }
        }

        finish(SUCCESS, json::object(), nullptr);
        return;
    }

    if (type == TASK_CANCEL) {
        if (req.contains("tasks") && req["tasks"].is_array()) {
            boost::unique_lock lock(m_ftps_mutex);
            for (auto const &task : req["tasks"])
                m_ftps_cancelled.insert(task.get<boost::uint32_t>());
        }
        json resp;
        resp["tasks"] = req.value("tasks", json::array());
        finish(SUCCESS, resp, nullptr);
        return;
    }

    // FTPS mode is deliberately exclusive. Unsupported browser operations
    // fail here and are never forwarded to the :6000 backend.
    BOOST_LOG_TRIVIAL(warning) << "[StorageFTPS] unsupported request type=" << type;
    finish(API_VERSION_UNSUPPORT, json::object(), nullptr);
}

void PrinterFileSystem::RequestFtpsUpload()
{
    std::shared_ptr<UploadFile> upload_file;
    {
        boost::unique_lock lock(m_mutex);
        upload_file = m_upload_file;
    }
    if (!upload_file)
        return;

    const boost::uint32_t seq = m_ftps_sequence.fetch_add(1);
    m_upload_seq = seq;
    upload_file->flags |= FF_UPLOADING;
    {
        boost::unique_lock lock(m_ftps_mutex);
        m_ftps_active.insert(seq);
    }

    boost::thread worker([w = weak_from_this(), seq, upload_file] {
        auto self = w.lock();
        if (!self)
            return;

        std::string prefix;
        std::string label;
        std::string error;
        if (!self->EnsureFtpsStorage(prefix, label, error)) {
            self->SendChangedEvent(EVT_UPLOAD_CHANGED, FF_UPLOADCANCEL, error,
                                   STORAGE_UNAVAILABLE);
            self->FinishFtpsRequest(seq);
            return;
        }

        if (upload_file->select_storage == "internal" ||
            upload_file->select_storage == "emmc") {
            self->SendChangedEvent(EVT_UPLOAD_CHANGED, FF_UPLOADCANCEL,
                                   "FTPS exposes external storage only.",
                                   STORAGE_UNAVAILABLE);
            self->FinishFtpsRequest(seq);
            return;
        }

        std::string host;
        std::string user;
        std::string password;
        {
            boost::unique_lock lock(self->m_ftps_mutex);
            host = self->m_ftps_host;
            user = self->m_ftps_user;
            password = self->m_ftps_password;
        }
        BambuFtps::Client client(host, user, password);
        const std::string remote = ftps_join_path(prefix, upload_file->name);
        error = client.upload(
            upload_file->path, remote,
            [self, seq, upload_file](std::uint64_t now, std::uint64_t total) {
                if (self->IsFtpsCancelled(seq))
                    return false;
                upload_file->size = static_cast<boost::uint32_t>(
                    std::min<std::uint64_t>(now, std::numeric_limits<boost::uint32_t>::max()));
                const int progress = total
                    ? static_cast<int>(std::min<std::uint64_t>(99, now * 100 / total))
                    : 0;
                self->SendChangedEvent(EVT_UPLOADING, progress);
                return true;
            });

        const bool cancelled = self->IsFtpsCancelled(seq) || error == "cancelled";
        if (error.empty()) {
            upload_file->flags &= ~FF_UPLOADING;
            upload_file->flags |= FF_UPLOADDONE;
            self->SendChangedEvent(EVT_UPLOADING, 100);
            self->SendChangedEvent(EVT_UPLOAD_CHANGED, FF_UPLOADDONE);
            self->PostCallback([w] {
                if (auto fs = w.lock())
                    fs->ListAllFiles();
            });
        } else {
            upload_file->flags &= ~FF_UPLOADING;
            upload_file->flags |= FF_UPLOADCANCEL;
            self->SendChangedEvent(EVT_UPLOAD_CHANGED, FF_UPLOADCANCEL, error,
                                   cancelled ? ERROR_CANCEL : SEND_ERR);
        }

        {
            boost::unique_lock lock(self->m_mutex);
            if (self->m_upload_file == upload_file)
                self->m_upload_file.reset();
        }
        self->FinishFtpsRequest(seq);
    });
    worker.detach();
}

boost::uint32_t PrinterFileSystem::SendRequest(int type, json const &req, callback_t2 const &callback,const std::string& param)
{
    if (m_session.tunnel == nullptr) {
        Retry();
        callback(ERROR_PIPE, json(), nullptr);
        return 0;
    }
    boost::uint32_t seq  = m_sequence + m_callbacks.size();
    json root;
    root["cmdtype"] = type;
    root["sequence"] = seq;
    root["req"] = req;
    std::ostringstream oss;
    oss << root;

    if (!param.empty()) {
        oss << "\n\n";
        oss << param;
    }
    // OutputDebugStringA(oss.str().c_str());
    // OutputDebugStringA("\n");
    auto               msg = oss.str();
    boost::unique_lock l(m_mutex);
    m_messages.push_back(msg);
    m_callbacks.push_back(callback);
    m_cond.notify_all();
    return seq;
}

void PrinterFileSystem::InstallNotify(int type, callback_t2 const &callback)
{
    type -= NOTIFY_FIRST;
    if (m_notifies.size() <= size_t(type)) m_notifies.resize(type + 1);
    m_notifies[type] = callback;
}

void PrinterFileSystem::CancelRequest(boost::uint32_t seq) { CancelRequests({seq}); }

void PrinterFileSystem::CancelRequests(std::vector<boost::uint32_t> const &seqs)
{
    json req;
    json arr;
    for (auto seq : seqs)
        arr.push_back(seq);
    req["tasks"] = arr;
    SendRequest(TASK_CANCEL, req, [this](int result, json const &resp, unsigned char const *) -> int {
        if (result != 0) return result;
        json tasks = resp["tasks"];
        std::vector<boost::uint32_t> seqs;
        for (auto &f : tasks) seqs.push_back(f);
        CancelRequests2(seqs);
        return 0;
    });
}

void PrinterFileSystem::CancelRequests2(std::vector<boost::uint32_t> const &seqs)
{
    std::vector<std::pair<boost::uint32_t, callback_t2>> callbacks;
    boost::unique_lock      l(m_mutex);
    for (auto &f : seqs) {
        boost::uint32_t seq = f;
        seq -= m_sequence;
        if (size_t(seq) >= m_callbacks.size()) continue;
        auto &c = m_callbacks[seq];
        if (c == nullptr) continue;
        callbacks.emplace_back(f, c);
        c = nullptr;

        // erase m_produce_message_cb
        if (m_produce_message_cb_map.find(seq) != m_produce_message_cb_map.end())
            m_produce_message_cb_map.erase(seq);
    }
    while (!m_callbacks.empty() && m_callbacks.front() == nullptr) {
        m_callbacks.pop_front();
        ++m_sequence;
    }
    l.unlock();
    for (auto &c : callbacks) {
        wxLogInfo("PrinterFileSystem::CancelRequests2: %u\n", c.first);
        c.second(ERROR_CANCEL, json(), nullptr);
    }
}

void PrinterFileSystem::RecvMessageThread()
{
    Bambu_Sample sample;
    boost::unique_lock l(m_mutex);
    Reconnect(l, 0);
    while (true) {
        if (m_stopped && (m_session.owner == nullptr || (m_messages.empty() && m_callbacks.empty()))) {
            Reconnect(l, 0); // Close and wait start again
            if (m_session.owner == nullptr) {
                // clear callbacks first
                auto callbacks(std::move(m_callbacks));
                break;
            }
        }
        if (m_messages.empty() && !m_produce_message_cb_map.empty()) {
            auto it = m_produce_message_cb_map.begin();
            while(it != m_produce_message_cb_map.end()) {
                std::string     msg;
                auto            prodeuce_message_cb = it->second;
                l.unlock();
                int res = prodeuce_message_cb(msg);
                l.lock();
                if (res == CONTINUE || res == SUCCESS) {
                    m_messages.emplace_back(msg);
                    if (res == SUCCESS) {
                        it = m_produce_message_cb_map.erase(it);
                        continue;
                    }
                    it++;
                } else {
                    int seq2 = it->first - m_sequence;
                    // erase it
                    it = m_produce_message_cb_map.erase(it);
                    if (size_t(seq2) >= m_callbacks.size())
                        continue;
                    auto c = m_callbacks[seq2];
                    if (c == nullptr)
                        continue;;
                    m_callbacks[seq2] = nullptr;
                    if (seq2 == 0) {
                        // if produce message return error, erase callback and sequence should plus
                        while (!m_callbacks.empty() && m_callbacks.front() == nullptr) {
                            m_callbacks.pop_front();
                            ++m_sequence;
                        }
                    }

                    l.unlock();
                    c(res, json(), nullptr);
                    l.lock();
                }
            }
        }
        if (!m_messages.empty()) {
            auto & msg = m_messages.front();
            // OutputDebugStringA(msg.c_str());
            // OutputDebugStringA("\n");
            wxLogInfo("PrinterFileSystem::SendRequest >>>: \n%s\n", wxString::FromUTF8(msg));
            l.unlock();
            int n = Bambu_SendMessage(m_session.tunnel, CTRL_TYPE, msg.c_str(), msg.length());
            l.lock();
            if (n == 0)
                m_messages.pop_front();
            else if (n != Bambu_would_block) {
                Reconnect(l, n);
                continue;
            }
        }
        l.unlock();
        int n = Bambu_ReadSample(m_session.tunnel, &sample);
        l.lock();
        if (n == 0) {
            HandleResponse(l, sample);
        } else if (n == Bambu_stream_end) {
            m_stopped = true;
            Reconnect(l, m_status == ListSyncing ? ERROR_RES_BUSY : ERROR_PIPE);
        } else if (n == Bambu_would_block) {
            m_cond.timed_wait(l, boost::posix_time::milliseconds(m_messages.empty() && m_callbacks.empty() ? 1000 : 20));
        } else {
            Reconnect(l, n);
        }
    } // while
}

void PrinterFileSystem::HandleResponse(boost::unique_lock<boost::mutex> &l, Bambu_Sample const &sample)
{
    unsigned char const *end      = sample.buffer + sample.size;
    unsigned char const *json_end = (unsigned char const *) memchr(sample.buffer, '\n', sample.size);
    while (json_end && json_end + 3 < end && json_end[1] != '\n') json_end = (unsigned char const *) memchr(json_end + 2, '\n', end - json_end - 2);
    if (json_end)
        json_end += 2;
    else
        json_end = end;
    std::string msg((char const *) sample.buffer, json_end - sample.buffer);
    json        root;
    // OutputDebugStringA(msg.c_str());
    // OutputDebugStringA("\n");
    wxLogInfo("PrinterFileSystem::HandleResponse <<<: \n%s\n", wxString::FromUTF8(msg));
    std::istringstream iss(msg);
    int                cmd    = 0;
    int                seq    = -1;
    int                result = 0;
    json               resp;
    try {
        iss >> root;
        if (!root["result"].is_null()) {
            result = root["result"];
            seq    = root["sequence"];
            resp   = root["reply"];
        } else {
            // maybe notify
            cmd  = root["cmdtype"];
            seq  = root["sequence"];
            resp = root["notify"];
        }
    } catch (...) {
        result = ERROR_JSON;
        return;
    }
    if (cmd > 0) {
        if (cmd < NOTIFY_FIRST) return;
        cmd -= NOTIFY_FIRST;
        if (size_t(cmd) >= m_notifies.size()) return;
        auto n = m_notifies[cmd];
        l.unlock();
        n(result, resp, json_end);
        l.lock();
    } else {
        int seq2 = seq - m_sequence;
        if (size_t(seq2) >= m_callbacks.size()) return;
        auto c = m_callbacks[seq2];
        if (c == nullptr) return;
        l.unlock();
        int result2 = c(result, resp, json_end);
        l.lock();
        if (result2 != CONTINUE) {
            int seq2          = seq - m_sequence;
            m_callbacks[seq2] = callback_t2();
            if (seq2 == 0) {
                while (!m_callbacks.empty() && m_callbacks.front() == nullptr) {
                    m_callbacks.pop_front();
                    ++m_sequence;
                }
            }
            if (result == CONTINUE) {
                l.unlock();
                CancelRequest(seq);
                l.lock();
            }

            // error should erase m_produce_message_cb
            if (m_produce_message_cb_map.find(seq2) != m_produce_message_cb_map.end()) {
                m_produce_message_cb_map.erase(seq2);
            }
        }
    }
}

void PrinterFileSystem::Reconnect(boost::unique_lock<boost::mutex> &l, int result)
{
    if (m_session.tunnel) {
        auto tunnel = m_session.tunnel;
        m_session.tunnel = nullptr;
        wxLogMessage("PrinterFileSystem::Reconnect close %d", result);
        l.unlock();
        Bambu_Close(tunnel);
        Bambu_Destroy(tunnel);
        l.lock();
    }
    if (m_session.owner == nullptr)
        return;
    json r;
    while(!m_callbacks.empty()) {
        auto c = m_callbacks.front();
        m_callbacks.pop_front();
        ++m_sequence;
        if (c) c(result, r, nullptr);
    }
    m_messages.clear();
    if (result)
        m_cond.timed_wait(l, boost::posix_time::seconds(10));


    while (true) {
        while (m_stopped) {
            if (m_session.owner == nullptr)
                return;
           m_status = Status::Reconnecting;
           SendChangedEvent(EVT_STATUS_CHANGED, m_status);
           m_cond.wait(l);
        }
        wxLogMessage("PrinterFileSystem::Reconnect Initializing");
        m_status = Status::Initializing;
        m_last_error = 0;
        SendChangedEvent(EVT_STATUS_CHANGED, m_status);
        // wait for url
        while (!m_stopped && m_messages.empty())
            m_cond.wait(l);
        if (m_stopped || m_messages.empty()) continue;
        std::string url = m_messages.front();
        m_messages.clear();
        if (url.size() < 2) {
            wxLogMessage("PrinterFileSystem::Reconnect Initialize failed: %s", wxString::FromUTF8(url));
            m_last_error = atoi(url.c_str());
            if (m_last_error == 0)
                m_stopped = true;
        } else {
            wxLogInfo("PrinterFileSystem::Reconnect Initialized: %s", wxString::FromUTF8(url));
            l.unlock();
            m_status = Status::Connecting;
            wxLogMessage("PrinterFileSystem::Reconnect Connecting");
            SendChangedEvent(EVT_STATUS_CHANGED, m_status);
            Bambu_Tunnel tunnel = nullptr;
            int ret = Bambu_Create(&tunnel, url.c_str());
            if (ret == 0) {

                Bambu_SetLogger(tunnel, DumpLog, this);
                ret = Bambu_Open(tunnel);
            }

            if (ret == 0)
            {
                auto                             start_time = boost::posix_time::microsec_clock::universal_time();
                boost::posix_time::time_duration timeout    = boost::posix_time::seconds(3);
                do{
                    ret = Bambu_StartStreamEx ? Bambu_StartStreamEx(tunnel, CTRL_TYPE) : Bambu_StartStream(tunnel, false);
                    if (ret == Bambu_would_block)
                        boost::this_thread::sleep(boost::posix_time::milliseconds(100));

                     auto now = boost::posix_time::microsec_clock::universal_time();
                    if (now - start_time > timeout) {
                        BOOST_LOG_TRIVIAL(warning) << "StartStream timeout after 5 seconds.";
                        break;
                    }

                } while (ret == Bambu_would_block && !m_stopped);
            }
            l.lock();
            if (ret == 0) {
                m_session.tunnel = tunnel;
                wxLogMessage("PrinterFileSystem::Reconnect Connected");
                break;
            } else if (ret == 1) {
                m_stopped = true;
                ret = ERROR_RES_BUSY;
            }
            if (tunnel) {
                Bambu_Close(tunnel);
                Bambu_Destroy(tunnel);
            }
            m_last_error = ret;
        }
        wxLogMessage("PrinterFileSystem::Reconnect Failed");
        m_status = Status::Failed;

        SendChangedEvent(EVT_STATUS_CHANGED, m_status, "", url.size() < 2 ? 1 : m_last_error);
        m_cond.timed_wait(l, boost::posix_time::seconds(10));
    }

#ifdef PRINTER_FILE_SYSTEM_TEST
    PostCallback([this] { SendChangedEvent(EVT_FILE_CHANGED); });
#else
    PostCallback([this] {
        m_task_flags = 0;
        m_status     = Status::ListSyncing;
        SendChangedEvent(EVT_STATUS_CHANGED, m_status);
        });
#endif
}


#include <stdlib.h>
#if defined(_MSC_VER) || defined(_WIN32)
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#if defined(_MSC_VER) || defined(_WIN32)
static HMODULE module = NULL;
#else
static void* module = NULL;
#endif

static void* get_function(const char* name)
{
    void* function = nullptr;

    if (!module)
        return function;

#if defined(_MSC_VER) || defined(_WIN32)
    function = reinterpret_cast<void*>(GetProcAddress(module, name));
#else
    function = dlsym(module, name);
#endif

    if (!function) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", can not find function %1%") % name;
    }
    return function;
}

#define GET_FUNC(x) lib.x = reinterpret_cast<decltype(lib.x)>(get_function(#x))

StaticBambuLib &StaticBambuLib::get(BambuLib *copy)
{
    static StaticBambuLib lib;
    // first load the library

    if (lib.Bambu_Create)
        return lib;

    if (!module) {
        module = Slic3r::NetworkAgent::get_bambu_source_entry();
    }

    if (!module) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", can not Load Library";
    }

    GET_FUNC(Bambu_Create);
    GET_FUNC(Bambu_Open);
    GET_FUNC(Bambu_StartStream);
    GET_FUNC(Bambu_StartStreamEx);
    GET_FUNC(Bambu_GetStreamCount);
    GET_FUNC(Bambu_GetStreamInfo);
    GET_FUNC(Bambu_SendMessage);
    GET_FUNC(Bambu_ReadSample);
    GET_FUNC(Bambu_Close);
    GET_FUNC(Bambu_Destroy);
    GET_FUNC(Bambu_SetLogger);
    GET_FUNC(Bambu_FreeLogMsg);
    GET_FUNC(Bambu_Deinit);

    if (!lib.Bambu_Create) {
        lib.Bambu_Create = Fake_Bambu_Create;
        if (copy)
            lib.copies_.push_back(copy);
    }
    return lib;
}

void StaticBambuLib::reset()
{
    get().Bambu_Create = nullptr;
    auto &lib = get();
    for (auto c : lib.copies_)
        *c = lib;
}

void StaticBambuLib::release()
{
    if (auto f = get().Bambu_Deinit)
        f();
}

extern "C" BambuLib *bambulib_get() {
    return &StaticBambuLib::get(); }
