#pragma once

const char consoleHTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8" />
  <title>ESP32 OTA Console</title>
  <style>
    body {
      font-family: monospace;
      font-size: 18px;
      background: rgba(17, 17, 17, 0.89);
      color: #00FF00;
      margin: 0;
      padding: 0 40px;  /* left/right "margin" inside the page */
    }
    h2 {
      color: #ffffff;
    }
    #log {
      white-space: pre-wrap;
      background: #000;
      padding: 10px;
      border: 2px solid #555;
      height: 75vh;
      overflow-y: scroll;
    }
    #controls {
      margin-top: 10px;
    }
    button {
      padding: 6px 12px;
      font-family: monospace;
      font-size: 14px;
      background: #222;
      color: #eee;
      border: 1px solid #555;
      cursor: pointer;
      margin-right: 8px;
    }
    button:hover {
      background: rgba(232, 38, 38, 0.88);
    }
  </style>
</head>
<body>
  <h2>HB9IUU ESP32 Over-the-Air Console</h2>
  <div id="log">Loading logs...</div>

  <div id="controls">
    <button id="restartBtn">Restart ESP</button>
    <button id="clearBtn">Clear Console</button>
  </div>

  <script>
    async function fetchLogs() {
      try {
        const res = await fetch('/logs');
        const text = await res.text();
        const logDiv = document.getElementById('log');
        const isAtBottom = logDiv.scrollTop + logDiv.clientHeight >= logDiv.scrollHeight - 5;
        logDiv.textContent = text;
        if (isAtBottom) {
          logDiv.scrollTop = logDiv.scrollHeight;
        }
      } catch (e) {
        console.error(e);
      }
    }

    async function restartESP() {
      if (!confirm('Really restart ESP?')) return;
      try {
        await fetch('/restart', { method: 'POST' });
        alert('Restart command sent. The page will stop responding for a few seconds.');
      } catch (e) {
        console.error(e);
        alert('Error sending restart command.');
      }
    }

    async function clearConsole() {
      try {
        await fetch('/clearlogs', { method: 'POST' });
        const logDiv = document.getElementById('log');
        logDiv.textContent = '';
      } catch (e) {
        console.error(e);
        alert('Error clearing console.');
      }
    }

    document.addEventListener('DOMContentLoaded', () => {
      document.getElementById('restartBtn').addEventListener('click', restartESP);
      document.getElementById('clearBtn').addEventListener('click', clearConsole);
      fetchLogs();
      setInterval(fetchLogs, 1000);
    });
  </script>
</body>
</html>
)rawliteral";
