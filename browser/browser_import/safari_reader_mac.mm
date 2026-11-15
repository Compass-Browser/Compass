// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/safari_reader.h"

#import <Foundation/Foundation.h>

#include <climits>

#include "base/apple/foundation_util.h"
#include "base/strings/sys_string_conversions.h"

namespace compass::browser_import {

namespace {

std::optional<base::Value> ToValue(id object, int depth) {
  if (depth > 64) {
    return std::nullopt;
  }
  if ([object isKindOfClass:[NSDictionary class]]) {
    base::DictValue dict;
    for (id key in (NSDictionary*)object) {
      if (![key isKindOfClass:[NSString class]]) {
        continue;
      }
      if (std::optional<base::Value> value =
              ToValue(((NSDictionary*)object)[key], depth + 1)) {
        dict.Set(base::SysNSStringToUTF8(key), std::move(*value));
      }
    }
    return base::Value(std::move(dict));
  }
  if ([object isKindOfClass:[NSArray class]]) {
    base::ListValue list;
    for (id element in (NSArray*)object) {
      if (std::optional<base::Value> value = ToValue(element, depth + 1)) {
        list.Append(std::move(*value));
      }
    }
    return base::Value(std::move(list));
  }
  if ([object isKindOfClass:[NSString class]]) {
    return base::Value(base::SysNSStringToUTF8(object));
  }
  if ([object isKindOfClass:[NSNumber class]]) {
    NSNumber* number = object;
    if (CFGetTypeID((__bridge CFTypeRef)number) == CFBooleanGetTypeID()) {
      return base::Value(static_cast<bool>(number.boolValue));
    }
    if (CFNumberIsFloatType((__bridge CFNumberRef)number)) {
      return base::Value(number.doubleValue);
    }
    const long long integer = number.longLongValue;
    if (integer >= INT_MIN && integer <= INT_MAX) {
      return base::Value(static_cast<int>(integer));
    }
    return base::Value(static_cast<double>(integer));
  }
  if ([object isKindOfClass:[NSDate class]]) {
    return base::Value(((NSDate*)object).timeIntervalSinceReferenceDate);
  }
  return std::nullopt;
}

}  // namespace

std::optional<base::DictValue> ReadPlist(const base::FilePath& path) {
  @autoreleasepool {
    NSData* data = [NSData
        dataWithContentsOfFile:base::apple::FilePathToNSString(path)];
    if (!data) {
      return std::nullopt;
    }
    id plist = [NSPropertyListSerialization
        propertyListWithData:data
                     options:NSPropertyListImmutable
                      format:nullptr
                       error:nil];
    if (!plist) {
      return std::nullopt;
    }
    std::optional<base::Value> value = ToValue(plist, 0);
    if (!value || !value->is_dict()) {
      return std::nullopt;
    }
    return std::move(*value).TakeDict();
  }
}

}  // namespace compass::browser_import
