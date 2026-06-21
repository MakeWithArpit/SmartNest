#pragma once
static const char PROVISION_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">

<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <title>SmartNest Setup</title>
    <style>
        * {
            box-sizing: border-box
        }

        body {
            margin: 0;
            font-family: system-ui, Segoe UI, Arial, sans-serif;
            background: #f5f7fb;
            color: #111827
        }

        .wrap {
            max-width: 420px;
            margin: 0 auto;
            padding: 28px 18px
        }

        h1 {
            font-size: 24px;
            text-align: center;
            margin: 0 0 6px
        }

        p {
            margin: 0 0 18px;
            text-align: center;
            color: #4b5563
        }

        .panel {
            background: #fff;
            border: 1px solid #e5e7eb;
            border-radius: 8px;
            padding: 18px;
            box-shadow: 0 8px 28px #0001
        }

        label {
            display: block;
            margin: 14px 0 6px;
            font-weight: 600
        }

        select,
        input,
        button {
            width: 100%;
            font: inherit;
            border-radius: 6px
        }

        select,
        input {
            height: 42px;
            border: 1px solid #cbd5e1;
            padding: 0 10px;
            background: #fff
        }

        button {
            height: 42px;
            border: 0;
            background: #174ea6;
            color: #fff;
            font-weight: 700;
            margin-top: 14px
        }

        button.secondary {
            background: #eef2f7;
            color: #111827;
            border: 1px solid #cbd5e1
        }

        button:disabled {
            opacity: .6
        }

        .row {
            display: grid;
            grid-template-columns: 1fr auto;
            gap: 8px
        }

        .row button {
            width: auto;
            margin-top: 0;
            padding: 0 12px
        }

        .msg {
            min-height: 22px;
            margin-top: 14px;
            color: #374151
        }

        .ok {
            color: #087443
        }

        .err {
            color: #b42318
        }

        .hide {
            display: none
        }

        .small {
            font-size: 13px;
            color: #6b7280;
            margin-top: 10px
        }
    </style>
</head>

<body>
    <main class="wrap">
        <h1>SmartNest Wi-Fi Setup</h1>
        <p>Select your Wi-Fi network and enter the password.</p>
        <section class="panel">
            <label for="nets">Network</label>
            <div class="row">
                <select id="nets">
                    <option>Scanning...</option>
                </select>
                <button class="secondary" id="scanBtn" onclick="scan()">Scan</button>
            </div>
            <label for="ssid">SSID</label>
            <input id="ssid" autocomplete="off" placeholder="Wi-Fi name">
            <label for="pass">Password</label>
            <input id="pass" type="password" autocomplete="current-password" placeholder="Wi-Fi password">
            <button id="connectBtn" onclick="connectWifi()">Connect</button>
            <div id="msg" class="msg"></div>
            <div class="small">Keep this page open until the device confirms connection.</div>
        </section>
    </main>
    <script>
        const nets = document.getElementById('nets'), ssid = document.getElementById('ssid'), pass = document.getElementById('pass'), msg = document.getElementById('msg'), connectBtn = document.getElementById('connectBtn'), scanBtn = document.getElementById('scanBtn');
        nets.onchange = () => { if (nets.value) ssid.value = nets.value };
        function setMsg(text, cls = '') { msg.className = 'msg ' + cls; msg.textContent = text }
        async function scan() {
            scanBtn.disabled = true; setMsg('Scanning...');
            try {
                const r = await fetch('/scan', { cache: 'no-store' }), list = await r.json();
                nets.innerHTML = '';
                if (!list.length) { nets.innerHTML = '<option value="">No networks found</option>'; setMsg('No networks found', 'err'); return }
                list.sort((a, b) => b.rssi - a.rssi).forEach(n => {
                    const o = document.createElement('option'); o.value = n.ssid; o.textContent = n.ssid + ' (' + n.rssi + ' dBm)'; nets.appendChild(o);
                });
                ssid.value = list[0].ssid; setMsg('Select a network and connect.');
            } catch (e) { nets.innerHTML = '<option value="">Scan failed</option>'; setMsg('Scan failed. Try again.', 'err') }
            finally { scanBtn.disabled = false }
        }
        async function connectWifi() {
            if (!ssid.value.trim()) { setMsg('SSID required.', 'err'); return }
            connectBtn.disabled = true; setMsg('Connecting...');
            const data = new URLSearchParams(); data.set('ssid', ssid.value.trim()); data.set('password', pass.value);
            try {
                const r = await fetch('/connect', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: data });
                if (!r.ok) throw new Error('connect failed');
                pollStatus();
            } catch (e) { connectBtn.disabled = false; setMsg('Connect request failed.', 'err') }
        }
        function pollStatus() {
            let tries = 0, t = setInterval(async () => {
                tries++;
                try {
                    const r = await fetch('/connect-status', { cache: 'no-store' }), s = await r.json();
                    if (s.status === 'connected') { clearInterval(t); setMsg('Connected. IP: ' + (s.ip || ''), 'ok'); return }
                    if (s.status === 'failed') { clearInterval(t); connectBtn.disabled = false; setMsg('Connection failed. Check password.', 'err'); return }
                    setMsg('Connecting... ' + tries);
                } catch (e) { }
                if (tries > 35) { clearInterval(t); connectBtn.disabled = false; setMsg('Timed out. Try again.', 'err') }
            }, 1000);
        }
        scan();
    </script>
</body>

</html>
)rawliteral";
