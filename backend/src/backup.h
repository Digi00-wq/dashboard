#pragma once

#include "config.h"

#include <string>

// Daily backups of DATA_DIR (collect.db via SQLite's online backup API,
// users.json, prefs.json) into BACKUP_DIR, keeping BACKUP_KEEP days.
// Owned by the "data" work package.
void start_backup_thread(const Config &cfg); // detached thread
bool backup_now(const Config &cfg, std::string &error); // CLI: --backup-now
