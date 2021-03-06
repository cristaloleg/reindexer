#include <fstream>
#include <mutex>

#include <thread>
#include "dbmanager.h"
#include "gason/gason.h"
#include "tools/fsops.h"
#include "tools/jsontools.h"
#include "tools/logger.h"
#include "tools/stringstools.h"

namespace reindexer_server {
DBManager::DBManager(const string &dbpath, bool noSecurity) : dbpath_(dbpath), noSecurity_(noSecurity) {}

Error DBManager::Init() {
	auto status = readUsers();
	if (!status.ok() && !noSecurity_) {
		return status;
	}

	vector<fs::DirEntry> foundDb;
	if (fs::ReadDir(dbpath_, foundDb) < 0) {
		return Error(errParams, "Can't read reindexer dir %s", dbpath_);
	}

	for (auto &de : foundDb) {
		if (de.isDir && validateObjectName(de.name)) {
			auto status = loadOrCreateDatabase(de.name);
			if (!status.ok()) {
				reindexer::logPrintf(LogError, "Failed to open database '%s' - %s", de.name, status.what());
			}
		}
	}
	return 0;
}

Error DBManager::OpenDatabase(const string &dbName, AuthContext &auth, bool canCreate) {
	auto status = Login(dbName, auth);
	if (!status.ok()) {
		return status;
	}

	smart_lock<shared_timed_mutex> lck(mtx_);
	auto it = dbs_.find(dbName);
	if (it != dbs_.end()) {
		auth.db_ = it->second;
		return 0;
	}
	lck.unlock();
	lck = smart_lock<shared_timed_mutex>(mtx_, true);
	it = dbs_.find(dbName);
	if (it != dbs_.end()) {
		auth.db_ = it->second;
		return 0;
	}

	if (!canCreate) {
		return Error(errParams, "Database '%s' not found", dbName);
	}
	if (auth.role_ < kRoleOwner) {
		return Error(errForbidden, "Forbidden to create database %s", dbName);
	}

	if (!validateObjectName(dbName)) {
		return Error(errParams, "Database name contains invalid character. Only alphas, digits,'_','-, are allowed");
	}

	status = loadOrCreateDatabase(dbName);
	if (!status.ok()) {
		return status;
	}

	lck.unlock();
	return OpenDatabase(dbName, auth, false);
}

Error DBManager::loadOrCreateDatabase(const string &dbName) {
	string storagePath = fs::JoinPath(dbpath_, dbName);

	logPrintf(LogInfo, "Loading database %s", dbName);
	auto db = std::make_shared<reindexer::Reindexer>();
	auto status = db->Connect(storagePath);
	if (status.ok()) {
		dbs_[dbName] = db;
	}

	return status;
}

Error DBManager::DropDatabase(AuthContext &auth) {
	shared_ptr<Reindexer> db;
	auto status = auth.GetDB(kRoleOwner, &db);
	if (!status.ok()) {
		return status;
	}
	string dbName = auth.dbName_;

	std::unique_lock<shared_timed_mutex> lck(mtx_);
	auto it = dbs_.find(auth.dbName_);
	if (it == dbs_.end()) {
		return Error(errParams, "Database %s not found", dbName);
	}
	dbs_.erase(it);
	auth.ResetDB();

	fs::RmDirAll(fs::JoinPath(dbpath_, dbName));
	return 0;
}

vector<string> DBManager::EnumDatabases() {
	shared_lock<shared_timed_mutex> lck(mtx_);
	vector<string> dbs;
	for (auto &it : dbs_) dbs.push_back(it.first);
	return dbs;
}

Error DBManager::Login(const string &dbName, AuthContext &auth) {
	if (IsNoSecurity()) {
		auth.role_ = kRoleOwner;
		auth.dbName_ = dbName;
		return 0;
	}

	if (auth.role_ != kUnauthorized && dbName == auth.dbName_) {
		return 0;
	}

	auto it = users_.find(auth.login_);
	if (it == users_.end()) {
		return Error(errForbidden, "Unauthorized");
	}
	// TODO change to SCRAM-RSA
	if (it->second.hash != auth.password_) {
		return Error(errForbidden, "Unauthorized");
	}

	auth.role_ = kRoleNone;

	if (!dbName.empty()) {
		const UserRecord &urec = it->second;
		auth.role_ = kRoleNone;

		auto dbIt = urec.roles.find("*");
		if (dbIt != urec.roles.end()) {
			auth.role_ = dbIt->second;
		}

		dbIt = urec.roles.find(dbName);
		if (dbIt != urec.roles.end() && dbIt->second > auth.role_) {
			auth.role_ = dbIt->second;
		}
	}
	auth.dbName_ = dbName;
	logPrintf(LogInfo, "Authorized user '%s', to db '%s', role=%s\n", auth.login_, dbName, UserRoleName(auth.role_));

	return 0;
}

Error DBManager::readUsers() {
	users_.clear();
	string content;
	int res = fs::ReadFile(fs::JoinPath(dbpath_, "users.json"), content);
	if (res < 0) {
		return Error(errParams, "Can't read users.json file");
	}

	JsonAllocator jallocator;
	JsonValue jvalue;
	char *endp;
	res = jsonParse(&content[0], &endp, &jvalue, jallocator);
	if (res != JSON_OK) {
		return Error(errParams, "Error parsing users.json file");
	}

	for (auto ukey : jvalue) {
		UserRecord urec;
		urec.login = ukey->key;
		for (auto elem : ukey->value) {
			parseJsonField("hash", urec.hash, elem);
			if (!strcmp(elem->key, "roles")) {
				UserRole role = kRoleDataRead;
				for (auto pkey : elem->value) {
					string db = pkey->key;
					string strRole = pkey->value.toString();
					if (strRole == "data_read") {
						role = kRoleDataRead;
					} else if (strRole == "data_write") {
						role = kRoleDataWrite;
					} else if (strRole == "db_admin") {
						role = kRoleDBAdmin;
					} else if (strRole == "owner") {
						role = kRoleOwner;
					} else {
						logPrintf(LogWarning, "Skipping invalid role '%s' of user '%s' for db '%s'", strRole, urec.login, db);
					}
					urec.roles.emplace(db, role);
				}
			}
		}
		users_.emplace(urec.login, urec);
	}
	return 0;
}

const char *UserRoleName(UserRole role) {
	switch (role) {
		case kUnauthorized:
			return "unauthoried";
		case kRoleNone:
			return "none";
		case kRoleDataRead:
			return "data_read";
		case kRoleDataWrite:
			return "data_write";
		case kRoleDBAdmin:
			return "db_admin";
		case kRoleOwner:
			return "owner";
	}
	return "";
}

}  // namespace reindexer_server
