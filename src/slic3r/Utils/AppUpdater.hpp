#ifndef slic3r_AppUpdate_hpp_
#define slic3r_AppUpdate_hpp_

#include <boost/filesystem.hpp>
#include "libslic3r/Utils.hpp"

class boost::filesystem::path;

namespace Slic3r {

struct DownloadAppData
{
	std::string url;
	bool		start_after;
};

class AppUpdater
{
public:
	AppUpdater();
	~AppUpdater();
	AppUpdater(AppUpdater&&) = delete;
	AppUpdater(const AppUpdater&) = delete;
	AppUpdater& operator=(AppUpdater&&) = delete;
	AppUpdater& operator=(const AppUpdater&) = delete;

	// downloads app file
	void sync_download(const DownloadAppData& input_data);
	// downloads version file
	void sync_version(const std::string& version_check_url);
	void cancel();

	void		set_dest_path(const std::string& dest);
	std::string get_default_dest_folder();

	static std::string get_filename_from_url(const std::string& url);
	static std::string get_file_extension_from_url(const std::string& url);
private:
	struct priv;
	std::unique_ptr<priv> p;
};

wxDECLARE_EVENT(EVT_SLIC3R_VERSION_ONLINE, wxCommandEvent);
wxDECLARE_EVENT(EVT_SLIC3R_EXPERIMENTAL_VERSION_ONLINE, wxCommandEvent);
wxDECLARE_EVENT(EVT_SLIC3R_APP_DOWNLOAD_PROGRESS, wxCommandEvent);
} //namespace Slic3r 
#endif
