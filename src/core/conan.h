#pragma once

// Conan 1 local cache: every package in <conan home>\data becomes a search result.
//
//   <home>\data\<name>\<version>\<user>\<channel>\{export, export_source, source, build, package}
//
// "_" stands for no user / channel. Enter opens the folder to choose from when there is a choice
// (several versions, users or channels), otherwise the reference folder itself.

#include <string>
#include <vector>

namespace kamil {

struct ConanRef {
    std::wstring version, user, channel;
    std::wstring folder;  // ...\data\<name>\<version>\<user>\<channel>
    // "9.1.0@acme/stable", or "9.1.0" without user and channel.
    std::wstring display() const;
};

struct ConanPackage {
    std::wstring name;
    std::wstring folder;          // ...\data\<name>
    std::vector<ConanRef> refs;   // newest version first
};

// The Conan home (the folder that holds data\, profiles\, conan.conf): `setting` (dev.conan_home)
// when given, else %CONAN_USER_HOME%\.conan when that variable is set, else <profile>\.conan.
std::wstring conan_home(const std::wstring& setting, const std::wstring& conan_user_home, const std::wstring& profile);

// The data folder of a Conan home: [storage] path of <home>\conan.conf when set (relative to the
// home, "~" = `profile`), else <home>\data; the same under <home>\.conan when the setting names
// the parent of .conan (as CONAN_USER_HOME does). Empty when there is none.
std::wstring conan_data_dir(const std::wstring& home, const std::wstring& profile = {});

// The "path" of the [storage] section of a conan.conf text, as written ("./data", "D:\\conan");
// empty when it is not set.
std::wstring conan_storage_path(const std::string& conan_conf);

// Reads <data>\<name>\<version>\<user>\<channel> (folders only). Packages without any complete
// reference are listed too (their folder is still worth opening).
std::vector<ConanPackage> scan_conan_data(const std::wstring& data_dir, size_t max_refs = 20'000);

// The folder Enter opens: the package folder when it has several versions; the version folder
// when one version has several users; the user folder when one user has several channels;
// otherwise the reference folder (export, package, source ...).
std::wstring conan_open_folder(const ConanPackage& p);

// "9.1.0@acme/stable · 10.0.0@acme/testing (+3)" for the result's second line.
std::wstring conan_summary(const ConanPackage& p, size_t max_shown = 3);

// Version order: numeric parts compare as numbers ("10.0" > "9.1"), the rest as text.
// Negative, 0, positive like strcmp.
int compare_versions(const std::wstring& a, const std::wstring& b);

}  // namespace kamil
