#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>

// Album covers fetched from Home Assistant by the worker thread and served
// to browsers by the HTTP thread (/api/cover/<entity>). Browsers never see
// the HA URL or its access token.
class CoverCache {
public:
  struct Cover {
    std::string source; // HA entity_picture this was fetched from
    std::string mime;
    std::string bytes;
  };

  std::optional<Cover> get(const std::string &entity) const {
    std::lock_guard lock(m_);
    auto it = covers_.find(entity);
    if (it == covers_.end()) return std::nullopt;
    return it->second;
  }

  std::string source(const std::string &entity) const {
    std::lock_guard lock(m_);
    auto it = covers_.find(entity);
    return it == covers_.end() ? std::string() : it->second.source;
  }

  void put(const std::string &entity, Cover c) {
    std::lock_guard lock(m_);
    covers_[entity] = std::move(c);
  }

private:
  mutable std::mutex m_;
  std::map<std::string, Cover> covers_;
};
