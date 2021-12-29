// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/risk/hs-helper.h"  // hypserscan header

#include <unordered_map>
#include <vector>
#include <sstream>

namespace v8 {
namespace internal {
namespace risk {

HsHelper* HsHelper::m_instance = nullptr;

HsHelper& HsHelper::instance() {
  if (nullptr == HsHelper::m_instance) {
    // 加写锁
    // 判空
    HsHelper::m_instance = new HsHelper();
    // 解锁
  }
  return *HsHelper::m_instance;
}

HsHelper::~HsHelper() {
  if (HsHelper::m_instance != nullptr) {
    // 加写锁
    // 释放 hs 的句柄
    delete HsHelper::m_instance;
    HsHelper::m_instance = nullptr;
  }
}

int32_t HsHelper::SetPattern(const std::string& pattern) {
  auto it = m_patterns.find(pattern);
  if (it != m_patterns.end()) {
    return it->second;
  }

  std::vector<std::string> vctPats;
  Split(pattern, std::string("|"), vctPats);
  std::vector<const char*> pats;
  for (auto& it : vctPats) {
    pats.push_back(it.data());
  }

  hs_database_t* db = CompilePatterns(pats);
  if (nullptr == db) {
    return -1;
  }
  hs_scratch_t *scratch = nullptr;
  hs_error_t err = hs_alloc_scratch(db, &scratch);

  if (err != HS_SUCCESS) {
    return -1;
  }

  int32_t id = static_cast<int32_t>(m_hs.size()+1);
  HSInfo& info = m_hs[id];
  info.db = db;
  info.scratch = scratch;
  info.patterns.swap(vctPats);
  m_patterns[pattern] = id;
  return static_cast<int32_t>(id);
}

int32_t HsHelper::Test(int32_t patId, const std::string& haystack) {
  auto it = m_hs.find(patId);
  if (it == m_hs.end()) {
    return -1;
  }

  hs_error_t err = hs_scan(it->second.db, haystack.data(), static_cast<uint32_t>(haystack.size()), 0, it->second.scratch,
                           HsHelper::OnMatchOnce, &m_matchInfo);
  if (err != HS_SUCCESS && err != HS_SCAN_TERMINATED) {
    return -1;
  }
  return (m_matchInfo.id >= 0) ? 1 : 0;
}

size_t HsHelper::Split(const std::string& src, const std::string& delimit,
                       std::vector<std::string>& result) {
  size_t nEnd     = 0;
  size_t nBegin   = 0;
  int32_t delimit_len = static_cast<int32_t>(delimit.size());
  while (nEnd != src.npos) {
    nEnd = src.find(delimit, nBegin);
    if (nEnd == src.npos) {
      result.push_back(src.substr(nBegin, src.length() - nBegin));
    } else {
      result.push_back(src.substr(nBegin, nEnd - nBegin));
    }
    nBegin = nEnd + delimit_len;
  }

  return result.size();
}

hs_database_t* HsHelper::CompilePatterns(const std::vector<const char*> vctPats) {
  std::vector<unsigned> flags;
  std::vector<unsigned> ids;
  for (unsigned int i = 0; i < vctPats.size(); ++i) {
    ids.push_back(i);
    flags.push_back(HS_FLAG_SOM_LEFTMOST);
  }
  unsigned int mode = HS_MODE_BLOCK;
  hs_database_t* db = nullptr;
  hs_compile_error_t *compileErr = nullptr;

  hs_error_t err = hs_compile_multi(vctPats.data(), flags.data(), ids.data(), static_cast<uint32_t>(vctPats.size()),
                                    mode, nullptr, &db, &compileErr);
  if (err != HS_SUCCESS) {
    std::stringstream ss;
    if (compileErr->expression < 0) {
      ss << "code=" << err << " msg=" << compileErr->message;
    } else {
      ss << "code=" << err << " pattern=[" << vctPats[compileErr->expression] << "]"
         << " msg=" << compileErr->message;
    }
    m_errMsg = ss.str();
    return nullptr;
  }
  return db;
}

int HsHelper::OnMatchOnce(unsigned int id, unsigned long long from, unsigned long long to,
                          unsigned int flags, void *ctx) {
  if (ctx != nullptr) {
    MatchInfo* info = reinterpret_cast<MatchInfo*>(ctx);
    info->id = static_cast<int32_t>(id);
    info->from = static_cast<uint32_t>(from);
    info->size = static_cast<uint32_t>(to - from);
  }
  return -1;  // stop matching
}

}  // namespace risk
}  // namespace internal
}  // namespace v8

