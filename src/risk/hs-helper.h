// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_RISK_HS_HELPER_H_
#define V8_RISK_HS_HELPER_H_

#include <stdint.h>
#include <cctype>

#include <string.h>
#include <unordered_map>
#include <unordered_map>
#include <vector>

#include "v8.h"
#include "hs.h"  // hypserscan header

namespace v8 {
namespace internal {
namespace risk {

enum eRiskCode {
  ePatternIdNotFound = -101,
  eRegParamErr = -100,  // Invalid parameter, pattern-id MUST be int32
  // -1~-20: 是 hs 的错误
  eSucc = 0,
};

struct MatchInfo {
  int32_t id;
  uint32_t from;
  uint32_t size;
};

/**
 * hyperscan 接口封装, 非线程安全
 */
class HsHelper {
 public:
  static HsHelper& instance();
  ~HsHelper();
  int32_t SetPattern(const std::string& patterns);
  int32_t Test(int32_t patId, const std::string& haystack);
  const std::string& GetLastError() const { return m_errMsg; }
  HsHelper(const HsHelper&) = delete;

 private:
  HsHelper() {}
  size_t Split(const std::string& src, const std::string& delimit, std::vector<std::string>& result);
  hs_database_t* CompilePatterns(const std::vector<const char*> vctPats);
  static int OnMatchOnce(unsigned int id, unsigned long long from, unsigned long long to,
                         unsigned int flags, void *ctx);

 private:
  struct HSInfo {
    hs_database_t *db;
    hs_scratch_t *scratch;
    std::vector<std::string> patterns;
  };
  std::unordered_map<std::string, int32_t> m_patterns;
  std::unordered_map<int32_t, HSInfo> m_hs;
  std::string m_errMsg;
  MatchInfo m_matchInfo;
  static HsHelper *m_instance;
  // 换成读写锁吧
};

}  // namespace risk
}  // namespace internal
}  // namespace v8

#endif  // V8_RISK_HS_HELPER_H_
