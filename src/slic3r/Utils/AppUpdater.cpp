#include "AppUpdater.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <curl/curl.h>

#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/Utils/Http.hpp"

#ifdef _WIN32
#include <shellapi.h>
#endif // _WIN32


namespace Slic3r {

namespace {
	
#ifdef _WIN32
	bool run_file(const boost::filesystem::path& path)
	{
		// find updater exe
		if (boost::filesystem::exists(path)) {
			// run updater. Original args: /silent -restartapp prusa-slicer.exe -startappfirst

			// Using quoted string as mentioned in CreateProcessW docs, silent execution parameter.
			std::wstring wcmd = L"\"" + path.wstring();

			// additional information
			STARTUPINFOW si;
			PROCESS_INFORMATION pi;

			// set the size of the structures
			ZeroMemory(&si, sizeof(si));
			si.cb = sizeof(si);
			ZeroMemory(&pi, sizeof(pi));

			// start the program up
			if (CreateProcessW(NULL,   // the path
				wcmd.data(),    // Command line
				NULL,           // Process handle not inheritable
				NULL,           // Thread handle not inheritable
				FALSE,          // Set handle inheritance to FALSE
				0,              // No creation flags
				NULL,           // Use parent's environment block
				NULL,           // Use parent's starting directory 
				&si,            // Pointer to STARTUPINFO structure
				&pi             // Pointer to PROCESS_INFORMATION structure (removed extra parentheses)
			)) {
				// Close process and thread handles.
				CloseHandle(pi.hProcess);
				CloseHandle(pi.hThread);
				return true;
			}
			else {
				BOOST_LOG_TRIVIAL(error) << "Failed to run " << wcmd;
			}
		}
		return false;
	}

	bool open_folder(const boost::filesystem::path& path)
	{
		// this command can run the installer exe as well, but is it better than CreateProcessW?
		ShellExecuteW(NULL, NULL, path.parent_path().wstring().c_str(), NULL, NULL, SW_SHOWNORMAL);
		return true;
	}

#elif __linux__ || __APPLE__
	bool run_file(const boost::filesystem::path& path)
	{
		// find updater exe
		if (boost::filesystem::exists(path)) {
			std::string command = path.string();
			return std::system(command.c_str()) != -1;
		}
		return false;
	}
#endif 
}

wxDEFINE_EVENT(EVT_SLIC3R_VERSION_ONLINE, wxCommandEvent);
wxDEFINE_EVENT(EVT_SLIC3R_EXPERIMENTAL_VERSION_ONLINE, wxCommandEvent);
wxDEFINE_EVENT(EVT_SLIC3R_APP_DOWNLOAD_PROGRESS, wxCommandEvent);

struct AppUpdater::priv {
	priv();
	// Download file. What happens with the data is specified in completefn.
	bool http_get_file(const std::string& url
		, size_t size_limit
		, std::function<void(Http::Progress)> progress_fn
		, std::function<bool(std::string /*body*/, std::string& error_message)> completefn
		, std::string& error_message
	) const;
	// Reads version file online, prompts user with update dialog
	//bool check_version();
	// Download installer / app
	boost::filesystem::path download_file(const DownloadAppData& data) const;
	// Run file in m_last_dest_path
	bool run_downloaded_file(boost::filesystem::path path);

	void set_dest_path(const boost::filesystem::path& p) { m_user_dest_path = p; }
	void set_dest_path(boost::filesystem::path p) { m_user_dest_path = std::move(p); }

	void version_check(const std::string& version_check_url) const;
	void parse_version_string(const std::string& body) const;

	std::thread				m_thread;
	std::atomic_bool        m_cancel;
	boost::filesystem::path m_default_dest_folder;
	boost::filesystem::path m_user_dest_path;
};

AppUpdater::priv::priv() :
	m_cancel (false),
	m_default_dest_folder (boost::filesystem::path(data_dir()) / "cache")
{	
}

bool  AppUpdater::priv::http_get_file(const std::string& url, size_t size_limit, std::function<void(Http::Progress)> progress_fn, std::function<bool(std::string /*body*/, std::string& error_message)> complete_fn, std::string& error_message) const
{
	bool res = false;
	Http::get(url)
		.size_limit(size_limit)
		.on_progress([&, progress_fn](Http::Progress progress, bool& cancel) {
			cancel = this->m_cancel;
			progress_fn(std::move(progress));
			if (cancel) {
				error_message = "Download was canceled.";
				BOOST_LOG_TRIVIAL(error) << error_message;
			}	
		})
		.on_error([&](std::string body, std::string error, unsigned http_status) {
			error_message = GUI::format("Error getting: `%1%`: HTTP %2%, %3%",
				url,
				http_status,
				error);
			BOOST_LOG_TRIVIAL(error) << error_message;
		})
		.on_complete([&](std::string body, unsigned /* http_status */) {
			assert(complete_fn != nullptr);
			res = complete_fn(body, error_message);
		})
		.perform_sync();
	
	return res;
}

boost::filesystem::path AppUpdater::priv::download_file(const DownloadAppData& data) const
{
	boost::filesystem::path dest_path;
	size_t last_gui_progress = 0;
	if(!m_user_dest_path.empty())
		dest_path = m_user_dest_path;
	else {
		dest_path = m_default_dest_folder / AppUpdater::get_filename_from_url(data.url);
	}
	assert(!dest_path.empty());
	if (dest_path.empty())
	{
		BOOST_LOG_TRIVIAL(error) << "Download from " << data.url << " could not start. Destination path is empty.";
		return boost::filesystem::path();
	}
	std::string error_message;
	bool res = http_get_file(data.url, 70 * 1024 * 1024
		// on_progress
		, [&last_gui_progress](Http::Progress progress) {
			size_t gui_progress = progress.dltotal > 0 ? 100 * progress.dlnow / progress.dltotal : 0;
			printf("Download progress: %d\n", gui_progress);
			if (last_gui_progress < gui_progress && (last_gui_progress != 0 || gui_progress != 100)) {
				last_gui_progress = gui_progress;
				wxCommandEvent* evt = new wxCommandEvent(EVT_SLIC3R_APP_DOWNLOAD_PROGRESS);
				evt->SetString(GUI::from_u8(std::to_string(gui_progress)));
				GUI::wxGetApp().QueueEvent(evt);
			}
		}
		// on_complete
		, [dest_path](std::string body, std::string& error_message){
			boost::filesystem::path tmp_path = dest_path;
			tmp_path += format(".%1%%2%", get_current_pid(), ".download");
			try
			{
				boost::filesystem::fstream file(tmp_path, std::ios::out | std::ios::binary | std::ios::trunc);
				file.write(body.c_str(), body.size());
				file.close();
				boost::filesystem::rename(tmp_path, dest_path);
			}
			catch (const std::exception&)
			{
				BOOST_LOG_TRIVIAL(error) << "Failed to write and move " << tmp_path << " to " << dest_path;
				return false;
			}
			return true;
		}
		, error_message
	);
	if (!res)
	{
		BOOST_LOG_TRIVIAL(error) << "Download from " << data.url << " to " << dest_path.string() << "failed";
		// TODO: send error_message to UI
		return boost::filesystem::path();
	}
	
	return std::move(dest_path);
}

bool AppUpdater::priv::run_downloaded_file(boost::filesystem::path path)
{
	assert(!path.empty());
#ifdef _WIN32
	bool res = run_file(path);
	BOOST_LOG_TRIVIAL(error) << "Running "<< path.string() << " was " << res;
	return res;
#elif __linux__
	bool res = run_file(path);
	BOOST_LOG_TRIVIAL(error) << "Running "<< path.string() << " was " << res;
	return res;
#else
	return false;
#endif

}

void AppUpdater::priv::version_check(const std::string& version_check_url) const
{
	assert(!version_check_url.empty());
	std::string error_message;
	bool res = http_get_file(version_check_url, 256
		// on_progress
		, [](Http::Progress progress) {}
		// on_complete
		, [&](std::string body, std::string& error_message) {
			boost::trim(body);
			parse_version_string(body);
			return true;
		}
		, error_message
	);
}

void AppUpdater::priv::parse_version_string(const std::string& body) const
{
	// release version
	std::string version;
	const auto first_nl_pos = body.find_first_of("\n\r");
	if (first_nl_pos != std::string::npos)
		version = body.substr(0, first_nl_pos);
	else
		version = body;
	boost::optional<Semver> release_version = Semver::parse(version);
	if (!release_version) {
		BOOST_LOG_TRIVIAL(error) << format("Received invalid contents from `%1%`: Not a correct semver: `%2%`", SLIC3R_APP_NAME, version);
		return;
	}
	BOOST_LOG_TRIVIAL(info) << format("Got %1% online version: `%2%`. Sending to GUI thread...", SLIC3R_APP_NAME, version);
	wxCommandEvent* evt = new wxCommandEvent(EVT_SLIC3R_VERSION_ONLINE);
	evt->SetString(GUI::from_u8(version));
	GUI::wxGetApp().QueueEvent(evt);

	// alpha / beta version
	std::vector<std::string> prerelease_versions;
	size_t nexn_nl_pos = first_nl_pos;
	while (nexn_nl_pos != std::string::npos && body.size() > nexn_nl_pos + 1) {
		const auto last_nl_pos = nexn_nl_pos;
		nexn_nl_pos = body.find_first_of("\n\r", last_nl_pos + 1);
		std::string line;
		if (nexn_nl_pos == std::string::npos)
			line = body.substr(last_nl_pos + 1);
		else
			line = body.substr(last_nl_pos + 1, nexn_nl_pos - last_nl_pos - 1);

		// alpha
		if (line.substr(0, 6) == "alpha=") {
			version = line.substr(6);
			if (!Semver::parse(version)) {
				BOOST_LOG_TRIVIAL(error) << format("Received invalid contents for alpha release from `%1%`: Not a correct semver: `%2%`", SLIC3R_APP_NAME, version);
				return;
			}
			prerelease_versions.emplace_back(version);
			// beta
		}
		else if (line.substr(0, 5) == "beta=") {
			version = line.substr(5);
			if (!Semver::parse(version)) {
				BOOST_LOG_TRIVIAL(error) << format("Received invalid contents for beta release from `%1%`: Not a correct semver: `%2%`", SLIC3R_APP_NAME, version);
				return;
			}
			prerelease_versions.emplace_back(version);
		}
	}
	// find recent version that is newer than last full release.
	boost::optional<Semver> recent_version;
	for (const std::string& ver_string : prerelease_versions) {
		boost::optional<Semver> ver = Semver::parse(ver_string);
		if (ver && *release_version < *ver && ((recent_version && *recent_version < *ver) || !recent_version)) {
			recent_version = ver;
			version = ver_string;
		}
	}
	if (recent_version) {
		BOOST_LOG_TRIVIAL(info) << format("Got %1% online version: `%2%`. Sending to GUI thread...", SLIC3R_APP_NAME, version);
		wxCommandEvent* evt = new wxCommandEvent(EVT_SLIC3R_EXPERIMENTAL_VERSION_ONLINE);
		evt->SetString(GUI::from_u8(version));
		GUI::wxGetApp().QueueEvent(evt);
	}
}


AppUpdater::AppUpdater()
	:p(new priv())
{
}
AppUpdater::~AppUpdater()
{
	if (p && p->m_thread.joinable()) {
		// This will stop transfers being done by the thread, if any.
		// Cancelling takes some time, but should complete soon enough.
		p->m_cancel = true;
		p->m_thread.join();
	}
}
void AppUpdater::sync_download(const DownloadAppData& input_data)
{
	assert(p);
	// join thread first - it could have been in sync_version
	if (p->m_thread.joinable()) {
		// This will stop transfers being done by the thread, if any.
		// Cancelling takes some time, but should complete soon enough.
		p->m_cancel = true;
		p->m_thread.join();
	}
	p->m_cancel = false;
	p->m_thread = std::thread(
		[this, input_data]() {
			if (boost::filesystem::path dest_path = p->download_file(input_data); !dest_path.empty()){
				if (input_data.start_after)
					p->run_downloaded_file(std::move(dest_path));
#ifdef _WIN32
				else
					open_folder(dest_path);
#endif // _WIN32
			}
		});
}

void AppUpdater::sync_version(const std::string& version_check_url)
{
	assert(p);
	// join thread first - it could have been in sync_download
	if (p->m_thread.joinable()) {
		// This will stop transfers being done by the thread, if any.
		// Cancelling takes some time, but should complete soon enough.
		p->m_cancel = true;
		p->m_thread.join();
	}
	p->m_cancel = false;
	p->m_thread = std::thread(
		[this, version_check_url]() {
			p->version_check(version_check_url);
		});
}

void AppUpdater::cancel()
{
	p->m_cancel = true;
}

void AppUpdater::set_dest_path(const std::string& dest)
{
	p->m_user_dest_path = boost::filesystem::path(dest);
}
std::string AppUpdater::get_default_dest_folder()
{
	return p->m_default_dest_folder.string();
}

std::string AppUpdater::get_filename_from_url(const std::string& url)
{
	size_t slash = url.rfind('/');
	return (slash != std::string::npos ? url.substr(slash + 1) : url);
}

std::string AppUpdater::get_file_extension_from_url(const std::string& url)
{
	size_t dot = url.rfind('.');
	return (dot != std::string::npos ? url.substr(dot) : url);
}

} //namespace Slic3r 