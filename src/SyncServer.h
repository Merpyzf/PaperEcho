#pragma once
#include <WebServer.h>
#include <WiFi.h>
#include "DataStore.h"

class SyncServer {
public:
    SyncServer(DataStore &store)
        : _store(store), _server(80), _running(false), _newDataFlag(false),
          _syncInProgress(false), _syncExcerptCount(0) {}

    void begin() {
        _server.on("/api/status", HTTP_GET, [this]() { _handleStatus(); });
        _server.on("/api/sync", HTTP_POST, [this]() { _handleSync(); });
        _server.on("/api/sync/begin", HTTP_POST, [this]() { _handleSyncBegin(); });
        _server.on("/api/sync/batch", HTTP_POST, [this]() { _handleSyncBatch(); });
        _server.on("/api/sync/end", HTTP_POST, [this]() { _handleSyncEnd(); });
        _server.on("/api/data", HTTP_DELETE, [this]() { _handleDelete(); });
        _server.onNotFound([this]() { _handleNotFound(); });
        _server.begin();
        _running = true;
        Serial.println("HTTP server started on port 80");
    }

    void stop() {
        _server.stop();
        _running = false;
    }

    // Call in loop to handle incoming requests
    void handleClient() {
        if (_running) {
            _server.handleClient();
        }
    }

    bool isSyncInProgress() const { return _syncInProgress; }
    int getSyncExcerptCount() const { return _syncExcerptCount; }

    // Check and clear new data flag (set after successful sync)
    bool hasNewData() {
        if (_newDataFlag) {
            _newDataFlag = false;
            return true;
        }
        return false;
    }

private:
    DataStore &_store;
    WebServer _server;
    bool _running;
    bool _newDataFlag;

    // Batch sync state
    File _syncFile;
    bool _syncInProgress;
    int _syncExcerptCount;

    void _handleStatus() {
        String json = "{";
        json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
        json += "\"excerptCount\":" + String(_store.getExcerptCount()) + ",";
        json += "\"bookCount\":" + String(_store.getBookCount()) + ",";
        json += "\"sdFreeKB\":" + String(_store.getSDFreeKB()) + ",";
        json += "\"version\":\"1.0.0\"";
        json += "}";
        _server.send(200, "application/json", json);
    }

    void _handleSync() {
        if (!_server.hasArg("plain")) {
            _server.send(400, "application/json", "{\"error\":\"empty body\"}");
            return;
        }

        String body = _server.arg("plain");

        // Validate JSON briefly
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, body);
        if (err) {
            String errMsg = "{\"error\":\"invalid JSON: ";
            errMsg += err.c_str();
            errMsg += "\"}";
            _server.send(400, "application/json", errMsg);
            return;
        }

        // Save raw JSON to SD
        if (!_store.saveDataRaw(body)) {
            _server.send(500, "application/json", "{\"error\":\"SD write failed\"}");
            return;
        }

        _server.send(200, "application/json", "{\"status\":\"ok\"}");
        _newDataFlag = true;
    }

    // --- Batch sync: begin ---
    void _handleSyncBegin() {
        // Abort any previous incomplete sync
        if (_syncInProgress) {
            _syncFile.close();
            _syncInProgress = false;
        }

        if (!_server.hasArg("plain")) {
            _server.send(400, "application/json", "{\"error\":\"empty body\"}");
            return;
        }

        String body = _server.arg("plain");
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, body);
        if (err) {
            String errMsg = "{\"error\":\"invalid JSON: ";
            errMsg += err.c_str();
            errMsg += "\"}";
            _server.send(400, "application/json", errMsg);
            return;
        }

        // Delete old files (both formats)
        SD.remove("/xmnote/data.json");
        SD.remove("/xmnote/meta.json");
        SD.remove("/xmnote/excerpts.jsonl");

        // Write meta.json (books + reviewSettings, small file)
        if (!_store.saveMetaRaw(body)) {
            _server.send(500, "application/json", "{\"error\":\"SD write meta failed\"}");
            return;
        }

        // Open excerpts.jsonl for streaming writes
        _syncFile = SD.open("/xmnote/excerpts.jsonl", FILE_WRITE);
        if (!_syncFile) {
            _server.send(500, "application/json", "{\"error\":\"SD write failed\"}");
            return;
        }

        _syncInProgress = true;
        _syncExcerptCount = 0;

        Serial.println("Batch sync: begin");
        _server.send(200, "application/json", "{\"status\":\"ok\"}");
    }

    // --- Batch sync: batch ---
    void _handleSyncBatch() {
        if (!_syncInProgress) {
            _server.send(400, "application/json", "{\"error\":\"no sync in progress\"}");
            return;
        }

        if (!_server.hasArg("plain")) {
            _server.send(400, "application/json", "{\"error\":\"empty body\"}");
            return;
        }

        String body = _server.arg("plain");
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, body);
        if (err) {
            String errMsg = "{\"error\":\"invalid JSON: ";
            errMsg += err.c_str();
            errMsg += "\"}";
            _server.send(400, "application/json", errMsg);
            return;
        }

        JsonArray excerpts = doc["excerpts"];
        int batchCount = 0;
        for (JsonObject e : excerpts) {
            serializeJson(e, _syncFile);
            _syncFile.print("\n");
            _syncExcerptCount++;
            batchCount++;
        }
        _syncFile.flush();

        Serial.printf("Batch sync: wrote %d excerpts (total: %d)\n", batchCount, _syncExcerptCount);

        String resp = "{\"status\":\"ok\",\"totalExcerpts\":";
        resp += String(_syncExcerptCount);
        resp += "}";
        _server.send(200, "application/json", resp);
    }

    // --- Batch sync: end ---
    void _handleSyncEnd() {
        if (!_syncInProgress) {
            _server.send(400, "application/json", "{\"error\":\"no sync in progress\"}");
            return;
        }

        _syncFile.close();
        _syncInProgress = false;

        Serial.printf("Batch sync: end, total %d excerpts\n", _syncExcerptCount);

        String resp = "{\"status\":\"ok\",\"totalExcerpts\":";
        resp += String(_syncExcerptCount);
        resp += "}";
        _server.send(200, "application/json", resp);
        _newDataFlag = true;
    }

    void _handleDelete() {
        SD.remove("/xmnote/data.json");
        SD.remove("/xmnote/meta.json");
        SD.remove("/xmnote/excerpts.jsonl");
        _store.freeData();
        _newDataFlag = true;
        _server.send(200, "application/json", "{\"status\":\"ok\"}");
    }

    void _handleNotFound() {
        _server.send(404, "application/json", "{\"error\":\"not found\"}");
    }
};
