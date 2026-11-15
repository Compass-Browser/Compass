// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// The page scripts that apply a Boost on iOS (docs/design/boosts.md,
/// "iOS"): WKUserScripts in every frame, each checking the frame's
/// location against the Boost's pattern before doing anything.
public enum BoostScripts {
  /// The content world of isolated Boosts (and of every Boost's CSS).
  public static let worldName = "CompassBoosts"

  /// The CSS as a <style data-compass-boost="id">, added at document
  /// start (to <html> if there's no <head> yet) and moved to the end of
  /// <head> once the page is parsed, so it comes after the page's own
  /// styles. nil without CSS.
  public static func css(_ boost: Boost) -> String? {
    guard !boost.css.isEmpty, let match = boost.parsedMatch else { return nil }
    return """
      (function(){if(!\(match.javaScriptCondition))return;\
      var s=document.createElement('style');s.setAttribute('data-compass-boost',\(literal(boost.id)));\
      s.textContent=\(literal(boost.css));\
      function place(){var p=document.head||document.documentElement;if(!p)return false;p.appendChild(s);return true;}\
      if(!place()){new MutationObserver(function(_,o){if(place())o.disconnect();}).observe(document,{childList:true});}\
      document.addEventListener('DOMContentLoaded',place,{once:true});})();
      """
  }

  /// The JavaScript, in a function of its own, its errors in the console.
  /// nil without JavaScript.
  public static func js(_ boost: Boost) -> String? {
    guard !boost.js.isEmpty, let match = boost.parsedMatch else { return nil }
    // The code on lines of its own: a last-line comment can't swallow the
    // closing brace.
    return """
      (function(){if(!\(match.javaScriptCondition))return;try{(function(){
      \(boost.js)
      })();}catch(e){console.error('Compass Boost '+\(literal(boost.name))+':',e);}})();
      """
  }

  /// A JavaScript string literal for `text`.
  public static func literal(_ text: String) -> String {
    var out = "\""
    out.reserveCapacity(text.utf16.count + 2)
    for scalar in text.unicodeScalars {
      switch scalar {
      case "\"": out += "\\\""
      case "\\": out += "\\\\"
      case "\n": out += "\\n"
      case "\r": out += "\\r"
      case "\t": out += "\\t"
      // "</script>" can't end anything here, but "<" is cheap to escape.
      case "<": out += "\\u003c"
      case "\u{2028}": out += "\\u2028"
      case "\u{2029}": out += "\\u2029"
      default:
        if scalar.value < 0x20 {
          out += String(format: "\\u%04x", scalar.value)
        } else {
          out.unicodeScalars.append(scalar)
        }
      }
    }
    return out + "\""
  }
}
