// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// chrome-untrusted://live-folders/: runs each refresh the browser asks for in
// a dedicated Worker (worker.js) and relays its fetches and logs to the
// browser, which checks them. See docs/design/live-folders.md, "Runtime".

import {PageCallbackRouter, PageHandlerFactory, PageHandlerRemote} from '../live_folders.mojom-webui.js';
import type {FetchRequest, RunRequest, RunResult} from '../live_folders.mojom-webui.js';

// plugins/runtime/result.js and select.js, loaded by the page before this
// module.
interface ResultLib {
  normalizeResult(
      raw: unknown, limits: {maxItems?: number},
      options?: {preview: boolean, iconAllowed: (url: string) => boolean}):
      unknown;
}
interface SelectLib {
  applyToHtml(html: string, groups: object, baseUrl: string):
      Record<string, unknown[]>;
}
declare global {
  interface Window {
    CompassLive: {result: ResultLib, select: SelectLib};
  }
}

const callbackRouter = new PageCallbackRouter();
const handler = new PageHandlerRemote();
PageHandlerFactory.getRemote().createPageHandler(
    callbackRouter.$.bindNewPipeAndPassRemote(),
    handler.$.bindNewPipeAndPassReceiver());

const workers = new Map<number, Worker>();

function failure(code: string, message: string): RunResult {
  return {ok: false, resultJson: '', errorCode: code, errorMessage: message};
}

function run(request: RunRequest): Promise<{result: RunResult}> {
  return new Promise((resolve) => {
    const worker = new Worker('/live_folders/worker.js');
    workers.set(request.runId, worker);
    let done = false;
    const finish = (result: RunResult) => {
      if (done) {
        return;
      }
      done = true;
      clearTimeout(timer);
      worker.terminate();
      workers.delete(request.runId);
      resolve({result});
    };
    const timer = setTimeout(
        () => finish(failure(
            'timeout', `No result after ${request.timeoutMs} ms`)),
        request.timeoutMs);

    worker.onerror = (event: ErrorEvent) => {
      event.preventDefault();
      finish(failure('plugin_error', event.message || 'The plugin failed'));
    };
    worker.onmessage = async (event: MessageEvent) => {
      const message = event.data || {};
      switch (message.type) {
        case 'fetch': {
          const r = message.request || {};
          const fetchRequest: FetchRequest = {
            url: String(r.url ?? ''),
            method: String(r.method ?? 'GET'),
            headers: Object.fromEntries(Object.entries(r.headers || {}).map(
                ([k, v]) => [k, String(v)])),
            body: typeof r.body === 'string' ? r.body : null,
            includeCredentials: r.credentials === 'include',
            preferBrowser: r.via === 'browser',
          };
          // compass.fetch(url, {select}): the manifest's selector group on
          // the HTML, parsed here since the Worker has no DOM.
          const names: string[] =
              Array.isArray(r.select) ? r.select.map(String) : [];
          const unknown = names.find(name => !selectGroups[name]);
          if (unknown !== undefined) {
            worker.postMessage({
              type: 'fetchResult',
              id: message.id,
              ok: false,
              code: 'bad_request',
              message: `No preview.select group named ${unknown}`,
            });
            break;
          }
          const {result} = await handler.fetch(request.runId, fetchRequest);
          if (done) {
            return;
          }
          let groups: Record<string, unknown[]>|undefined;
          if (result.ok && names.length) {
            groups = window.CompassLive.select.applyToHtml(
                result.body,
                Object.fromEntries(names.map(name => [name, selectGroups[name]])),
                result.url);
          }
          worker.postMessage(
              result.ok ? {
                type: 'fetchResult',
                id: message.id,
                ok: true,
                response: {
                  status: result.status,
                  url: result.url,
                  headers: Object.fromEntries(Object.entries(result.headers)),
                  body: result.body,
                  groups,
                },
              } :
                          {
                            type: 'fetchResult',
                            id: message.id,
                            ok: false,
                            code: result.errorCode,
                            message: result.errorMessage,
                          });
          break;
        }
        case 'log':
          handler.log(request.runId, String(message.message));
          break;
        case 'done':
          try {
            // A preview's icons only from the hosts the browser allows (it
            // checks them again).
            const normalized = request.preview ?
                window.CompassLive.result.normalizeResult(
                    message.result, {},
                    {preview: true, iconAllowed: iconAllowed}) :
                window.CompassLive.result.normalizeResult(
                    message.result, {maxItems: request.maxItems});
            finish({
              ok: true,
              resultJson: JSON.stringify(normalized),
              errorCode: '',
              errorMessage: '',
            });
          } catch (e) {
            const error = e as {code?: string, message?: string};
            finish(failure(
                error.code || 'bad_result', error.message || String(e)));
          }
          break;
        case 'error':
          finish(failure(
              String(message.code || 'plugin_error'),
              String(message.message || '')));
          break;
      }
    };
    const iconAllowed = (url: string) => {
      let host: string;
      try {
        host = new URL(url).hostname.toLowerCase();
      } catch {
        return false;
      }
      return request.iconHosts.some((pattern: string) => {
        if (!pattern.startsWith('*.')) {
          return host === pattern;
        }
        const domain = pattern.slice(2);
        return host === domain || host.endsWith('.' + domain);
      });
    };
    const selectGroups: Record<string, object> =
        request.preview && request.selectJson ? JSON.parse(request.selectJson) :
                                                {};
    worker.postMessage({
      type: 'start',
      scripts: request.scripts,
      config: JSON.parse(request.configJson),
      env: JSON.parse(request.envJson),
      preview: request.preview,
      page: request.preview ? JSON.parse(request.pageJson || '{}') : null,
    });
  });
}

callbackRouter.run.addListener(run);
callbackRouter.cancel.addListener((runId: number) => {
  const worker = workers.get(runId);
  if (worker) {
    worker.terminate();
    workers.delete(runId);
  }
});
