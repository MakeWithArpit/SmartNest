#pragma once

static const char DASHBOARD_HTML[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=device-width, initial-scale=1.0" />
<title>SmartNest — Conference Room Controller</title>
<style>
  :root{
    --primary:#1A3A6B;
    --accent:#F47920;
    --bg:#F0F4F8;
    --surface:#FFFFFF;
    --text:#1C1C2E;
    --success:#27AE60;
    --danger:#E74C3C;
    --muted:#7F8C9A;
    --border:#E2E8F0;
    --shadow:0 2px 12px rgba(0,0,0,0.08);
    --radius-card:12px;
    --radius-btn:8px;
  }
  *{box-sizing:border-box;margin:0;padding:0}
  html,body{height:100%}
  body{
    font-family:system-ui,'Segoe UI',sans-serif;
    background:var(--bg);
    color:var(--text);
    font-size:14px;
    line-height:1.5;
    -webkit-font-smoothing:antialiased;
  }
  button{font-family:inherit;cursor:pointer;border:none;background:none;color:inherit}
  input{font-family:inherit;font-size:14px}
  a{color:inherit;text-decoration:none}

  /* ===== Login Page ===== */
  .login-wrap{
    min-height:100vh;display:flex;align-items:center;justify-content:center;
    padding:20px;background:var(--bg);
  }
  .login-card{
    width:100%;max-width:420px;background:var(--surface);
    border-radius:var(--radius-card);box-shadow:var(--shadow);
    padding:36px 32px;
  }
  .login-logo{
    font-size:32px;font-weight:800;color:var(--primary);text-align:center;
    letter-spacing:-0.5px;
  }
  .login-sub{
    text-align:center;color:var(--muted);font-size:13px;margin-top:6px;margin-bottom:28px;
  }
  .field{margin-bottom:16px}
  .field label{display:block;font-size:12px;font-weight:600;color:var(--text);margin-bottom:6px;text-transform:uppercase;letter-spacing:0.3px}
  .field input{
    width:100%;padding:11px 12px;border:1px solid var(--border);
    border-radius:var(--radius-btn);background:#fff;color:var(--text);outline:none;
    transition:border-color .15s;
  }
  .field input:focus{border-color:var(--primary)}
  .pw-wrap{position:relative}
  .pw-wrap input{padding-right:62px}
  .pw-toggle{
    position:absolute;right:8px;top:50%;transform:translateY(-50%);
    font-size:12px;font-weight:600;color:var(--primary);padding:6px 10px;border-radius:6px;
  }
  .pw-toggle:hover{background:#F0F4F8}
  .btn{
    display:inline-flex;align-items:center;justify-content:center;
    padding:11px 18px;border-radius:var(--radius-btn);font-weight:600;
    transition:all .15s;font-size:14px;border:1px solid transparent;white-space:nowrap;
  }
  .btn:disabled{opacity:.55;cursor:not-allowed}
  .btn-primary{background:var(--accent);color:#fff}
  .btn-primary:hover:not(:disabled){background:#dd6a16}
  .btn-outline{background:transparent;color:var(--accent);border-color:var(--accent)}
  .btn-outline:hover:not(:disabled){background:#FFF3E8}
  .btn-navy{background:transparent;color:var(--primary);border-color:var(--primary)}
  .btn-navy:hover:not(:disabled){background:#EAF0F8}
  .btn-danger{background:var(--danger);color:#fff}
  .btn-danger:hover:not(:disabled){background:#c93a2a}
  .btn-gray{background:transparent;color:var(--muted);border-color:var(--border)}
  .btn-gray:hover:not(:disabled){background:#F0F4F8}
  .btn-full{width:100%}
  .err-msg{
    background:#FDECEA;color:var(--danger);border:1px solid #F5C2BD;
    padding:10px 12px;border-radius:var(--radius-btn);font-size:13px;margin-bottom:14px;
  }
  .login-footer{
    text-align:center;color:var(--muted);font-size:11px;margin-top:24px;
  }

  /* ===== App Layout ===== */
  .app{display:flex;min-height:100vh}
  .sidebar{
    width:240px;background:var(--primary);color:#fff;
    display:flex;flex-direction:column;flex-shrink:0;
    position:sticky;top:0;height:100vh;
  }
  .sb-head{padding:22px 20px 18px;border-bottom:1px solid rgba(255,255,255,.08)}
  .sb-logo{font-size:22px;font-weight:800;letter-spacing:-0.3px}
  .sb-sublogo{font-size:11px;color:rgba(255,255,255,.65);margin-top:2px;text-transform:uppercase;letter-spacing:0.5px}
  .sb-nav{flex:1;padding:14px 10px;overflow-y:auto}
  .nav-item{
    display:block;width:100%;text-align:left;
    padding:10px 14px;border-radius:6px;color:rgba(255,255,255,.82);
    font-size:13.5px;font-weight:500;margin-bottom:2px;transition:all .12s;
  }
  .nav-item:hover{background:rgba(255,255,255,.08);color:#fff}
  .nav-item.active{background:var(--accent);color:#fff;font-weight:600}
  .sb-foot{padding:14px 20px;border-top:1px solid rgba(255,255,255,.08)}
  .sb-logout{
    color:#FF8A7E;font-weight:600;font-size:13px;padding:8px 0;
  }
  .sb-logout:hover{color:#fff}
  .sb-wifi{font-size:11px;color:rgba(255,255,255,.6);margin-top:10px}
  .sb-univ{font-size:11px;color:rgba(255,255,255,.45);margin-top:6px}

  .main{flex:1;display:flex;flex-direction:column;min-width:0}
  .topbar{
    background:#fff;border-bottom:1px solid var(--border);
    padding:14px 28px;display:flex;align-items:center;justify-content:space-between;
    position:sticky;top:0;z-index:10;
  }
  .page-title{font-size:18px;font-weight:700;color:var(--text)}
  .top-right{display:flex;align-items:center;gap:18px}
  .clock{font-family:'Segoe UI',system-ui;font-size:14px;font-weight:600;color:var(--primary);letter-spacing:0.5px}
  .conn{display:flex;align-items:center;gap:8px;font-size:13px;font-weight:500}
  .dot{width:9px;height:9px;border-radius:50%;background:var(--muted);transition:background .2s}
  .dot.ok{background:var(--success);box-shadow:0 0 0 3px rgba(39,174,96,.18)}
  .dot.bad{background:var(--danger);box-shadow:0 0 0 3px rgba(231,76,60,.18)}
  .spinner{
    width:14px;height:14px;border:2px solid var(--border);border-top-color:var(--primary);
    border-radius:50%;animation:spin .8s linear infinite;display:none;
  }
  .spinner.show{display:inline-block}
  @keyframes spin{to{transform:rotate(360deg)}}

  .content{padding:24px 28px;flex:1}
  .section{display:none}
  .section.active{display:block}
  .section h2{font-size:20px;font-weight:700;margin-bottom:6px;color:var(--text)}
  .section .sub{color:var(--muted);font-size:13px;margin-bottom:20px}

  .grid{display:grid;gap:16px}
  .grid-4{grid-template-columns:repeat(4,1fr)}
  .grid-3{grid-template-columns:repeat(3,1fr)}
  .grid-2{grid-template-columns:repeat(2,1fr)}

  .card{
    background:var(--surface);border-radius:var(--radius-card);
    box-shadow:var(--shadow);padding:18px;
  }
  .metric{border-top:3px solid var(--accent)}
  .metric .lbl{font-size:12px;font-weight:600;color:var(--muted);text-transform:uppercase;letter-spacing:0.5px}
  .metric .val{font-size:28px;font-weight:700;color:var(--primary);margin-top:8px}
  .metric .val .unit{font-size:14px;color:var(--muted);font-weight:500;margin-left:4px}

  .slave-card{display:flex;align-items:center;justify-content:space-between}
  .slave-card .name{font-weight:700;color:var(--text)}
  .slave-card .info{font-size:12px;color:var(--muted);margin-top:4px}
  .badge{
    display:inline-block;padding:3px 10px;border-radius:12px;
    font-size:11px;font-weight:700;letter-spacing:0.5px;
  }
  .b-on{background:#E6F7EE;color:var(--success)}
  .b-off{background:#F0F4F8;color:var(--muted)}
  .b-ok{background:#E6F7EE;color:var(--success)}
  .b-err{background:#FDECEA;color:var(--danger)}
  .b-warn{background:#FFF3E8;color:var(--accent)}

  /* Status pills */
  .pills{display:flex;flex-wrap:wrap;gap:10px;margin-top:8px}
  .pill{
    flex:1 1 140px;min-width:130px;background:var(--surface);
    border-radius:var(--radius-card);box-shadow:var(--shadow);padding:14px;text-align:center;
  }
  .pill .pname{font-size:12px;font-weight:600;color:var(--text)}
  .pill .pstate{
    display:inline-block;margin-top:8px;padding:5px 14px;border-radius:14px;
    font-size:12px;font-weight:700;letter-spacing:0.5px;
  }
  .pill .pstate.on{background:var(--success);color:#fff}
  .pill .pstate.off{background:#DDE3EA;color:var(--muted)}
  .pill .plocked{display:block;margin-top:6px;font-size:10px;font-weight:700;color:var(--accent);letter-spacing:0.5px}
  .pill .pruntime{margin-top:6px;font-size:11px;color:var(--muted);font-family:'Segoe UI',system-ui}

  /* Toggle */
  .toggle{
    position:relative;display:inline-block;width:48px;height:26px;flex-shrink:0;
  }
  .toggle input{opacity:0;width:0;height:0}
  .track{
    position:absolute;inset:0;background:#CBD3DC;border-radius:13px;
    transition:background .2s;cursor:pointer;
  }
  .track::before{
    content:"";position:absolute;left:3px;top:3px;width:20px;height:20px;
    background:#fff;border-radius:50%;transition:transform .2s;
    box-shadow:0 1px 3px rgba(0,0,0,.2);
  }
  .toggle input:checked + .track{background:var(--accent)}
  .toggle input:checked + .track::before{transform:translateX(22px)}
  .toggle.lock input:checked + .track{background:var(--accent)}

  /* Relay card */
  .relay-card{
    display:flex;flex-direction:column;gap:10px;
  }
  .relay-head{display:flex;justify-content:space-between;align-items:flex-start;gap:10px}
  .relay-name{font-weight:700;font-size:15px;color:var(--text)}
  .relay-state{font-size:12px;font-weight:700;letter-spacing:0.5px;margin-top:2px}
  .relay-state.on{color:var(--success)}
  .relay-state.off{color:var(--muted)}
  .relay-locked{
    display:inline-block;padding:2px 8px;border-radius:10px;
    background:#FFF3E8;color:var(--accent);font-size:10px;font-weight:700;letter-spacing:0.5px;margin-top:4px;
  }
  .relay-runtime{font-size:11px;color:var(--muted);font-family:'Segoe UI',system-ui;margin-top:auto}

  .divider{height:1px;background:var(--border);margin:24px 0}
  .action-bar{display:flex;gap:10px;flex-wrap:wrap;align-items:center}
  .action-bar .spacer{flex:1}
  .note{font-size:12px;color:var(--muted);margin-top:8px}

  /* AC section */
  .ac-card{max-width:560px;margin:0 auto}
  .ac-row{display:flex;align-items:center;justify-content:space-between;padding:14px 0;gap:14px;flex-wrap:wrap}
  .ac-row .lbl{font-weight:700;color:var(--text);min-width:120px}
  .ac-row + .ac-row{border-top:1px solid var(--border)}
  .temp-ctl{display:flex;align-items:center;gap:14px}
  .temp-val{font-size:22px;font-weight:700;color:var(--primary);min-width:70px;text-align:center}
  .temp-exact{display:flex;align-items:center;gap:8px;margin-top:10px;width:100%}
  .temp-exact label{font-size:12px;color:var(--muted);font-weight:600}
  .temp-exact input{
    width:80px;padding:8px 10px;border:1px solid var(--border);border-radius:var(--radius-btn);outline:none;
  }
  .seg{display:flex;flex-wrap:wrap;gap:6px}
  .seg button{
    padding:8px 14px;border:1px solid var(--border);border-radius:var(--radius-btn);
    font-size:12px;font-weight:600;color:var(--muted);background:#fff;
  }
  .seg button.active{background:var(--accent);color:#fff;border-color:var(--accent)}

  /* SD bar */
  .sd-row{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid var(--border)}
  .sd-row:last-child{border-bottom:none}
  .sd-row .lbl{color:var(--muted);font-weight:500}
  .sd-row .val{font-weight:700;color:var(--text)}
  .bar{display:flex;align-items:center;gap:12px;margin-top:14px}
  .bar-track{flex:1;height:10px;background:#E2E8F0;border-radius:6px;overflow:hidden}
  .bar-fill{height:100%;background:var(--accent);transition:width .3s;border-radius:6px}
  .bar-pct{font-size:13px;font-weight:700;color:var(--primary);min-width:42px;text-align:right}
  .mono-box{
    margin-top:14px;background:#F8FAFC;border:1px solid var(--border);
    border-radius:var(--radius-btn);padding:14px;font-family:'Consolas','Courier New',monospace;
    font-size:13px;color:var(--text);white-space:pre-wrap;
  }

  /* MQTT */
  .form-row{margin-bottom:14px}
  .form-row label{display:block;font-size:12px;font-weight:600;color:var(--text);margin-bottom:6px;text-transform:uppercase;letter-spacing:0.3px}
  .form-row input{
    width:100%;padding:10px 12px;border:1px solid var(--border);
    border-radius:var(--radius-btn);outline:none;background:#fff;
  }
  .form-row input:focus{border-color:var(--primary)}
  .form-row.tg{display:flex;justify-content:space-between;align-items:center;padding:10px 0}
  .form-row.tg label{margin-bottom:0}

  /* Toast */
  .toasts{position:fixed;top:20px;right:20px;z-index:1000;display:flex;flex-direction:column;gap:10px}
  .toast{
    padding:12px 18px;border-radius:var(--radius-btn);color:#fff;font-weight:500;font-size:13px;
    box-shadow:0 4px 14px rgba(0,0,0,.18);min-width:240px;max-width:360px;
    animation:slideIn .2s ease-out;
  }
  .toast.success{background:var(--success)}
  .toast.error{background:var(--danger)}
  .toast.info{background:var(--primary)}
  @keyframes slideIn{from{transform:translateX(20px);opacity:0}to{transform:translateX(0);opacity:1}}

  /* Modal */
  .overlay{
    position:fixed;inset:0;background:rgba(0,0,0,.5);
    display:none;align-items:center;justify-content:center;z-index:900;padding:20px;
  }
  .overlay.show{display:flex}
  .modal{
    background:#fff;border-radius:var(--radius-card);padding:24px;
    max-width:420px;width:100%;box-shadow:0 12px 40px rgba(0,0,0,.25);
  }
  .modal h3{font-size:17px;font-weight:700;color:var(--primary);margin-bottom:10px}
  .modal p{font-size:13.5px;color:var(--text);line-height:1.6;margin-bottom:16px}
  .modal-btns{display:flex;gap:10px;justify-content:flex-end}
  .modal input{
    width:100%;padding:10px 12px;border:1px solid var(--border);
    border-radius:var(--radius-btn);margin-bottom:14px;outline:none;
    font-family:'Consolas',monospace;
  }

  /* Connection banner */
  .conn-banner{
    position:fixed;top:0;left:0;right:0;background:var(--danger);color:#fff;
    text-align:center;padding:10px;font-weight:600;font-size:13px;z-index:1100;
    display:none;
  }
  .conn-banner.show{display:block}
  .reboot-banner{
    position:fixed;top:0;left:0;right:0;background:var(--primary);color:#fff;
    text-align:center;padding:12px;font-weight:600;font-size:14px;z-index:1100;
    display:none;
  }
  .reboot-banner.show{display:block}

  .loading-dots::after{content:"...";animation:dots 1.2s steps(4,end) infinite;display:inline-block;width:14px;text-align:left}
  @keyframes dots{0%,20%{content:""}40%{content:"."}60%{content:".."}80%,100%{content:"..."}}

  .sb-toggle{display:none}

  /* Responsive */
  @media (max-width: 1024px){
    .grid-4{grid-template-columns:repeat(2,1fr)}
    .grid-3{grid-template-columns:repeat(2,1fr)}
  }
  @media (max-width: 768px){
    .app{flex-direction:column}
    .sidebar{
      width:100%;height:auto;position:relative;
      flex-direction:row;align-items:center;padding:0;
    }
    .sb-head{flex:1;border-bottom:none;padding:14px 16px}
    .sb-nav{display:none;position:absolute;top:100%;left:0;right:0;background:var(--primary);z-index:50;padding:10px;border-top:1px solid rgba(255,255,255,.1)}
    .sb-nav.open{display:block}
    .sb-foot{display:none}
    .sb-toggle{display:block;padding:14px 18px;color:#fff;font-weight:700;font-size:14px}
    .topbar{padding:12px 16px}
    .page-title{font-size:15px}
    .clock{font-size:12px}
    .conn{font-size:11px}
    .content{padding:16px}
    .grid-4,.grid-3,.grid-2{grid-template-columns:1fr}
    .ac-row{flex-direction:column;align-items:flex-start}
  }
  @media (min-width: 769px) and (max-width:1024px){
    .relay-grid{grid-template-columns:repeat(2,1fr)}
  }
  @media (min-width: 1025px){
    .relay-grid{grid-template-columns:repeat(3,1fr)}
  }
  .relay-grid{display:grid;gap:16px;grid-template-columns:1fr}
</style>
</head>
<body>

<!-- ============ LOGIN PAGE ============ -->
<div id="loginPage" class="login-wrap">
  <div class="login-card">
    <div class="login-logo">SmartNest</div>
    <div class="login-sub">Conference Room Controller — Invertis University</div>
    <div id="loginErr" class="err-msg" style="display:none"></div>
    <form id="loginForm" autocomplete="off">
      <div class="field">
        <label for="loginUser">Username</label>
        <input type="text" id="loginUser" required />
      </div>
      <div class="field">
        <label for="loginPass">Password</label>
        <div class="pw-wrap">
          <input type="password" id="loginPass" required />
          <button type="button" class="pw-toggle" data-target="loginPass">Show</button>
        </div>
      </div>
      <button type="submit" class="btn btn-primary btn-full" id="loginBtn">Login</button>
    </form>
    <div class="login-footer">SmartNest v1.0 &nbsp;|&nbsp; Conference Room &nbsp;|&nbsp; Invertis University, Bareilly</div>
  </div>
</div>

<!-- ============ MAIN APP ============ -->
<div id="appPage" class="app" style="display:none">
  <aside class="sidebar">
    <div class="sb-head">
      <div class="sb-logo">SmartNest</div>
      <div class="sb-sublogo">Conference Room</div>
    </div>
    <button class="sb-toggle" id="sbToggle">Menu</button>
    <nav class="sb-nav" id="sbNav">
      <button class="nav-item active" data-sec="dashboard">Dashboard</button>
      <button class="nav-item" data-sec="lights">Lights and Relays</button>
      <button class="nav-item" data-sec="locks">Relay Locks</button>
      <button class="nav-item" data-sec="ac">AC Control</button>
      <button class="nav-item" data-sec="energy">Energy and Sensors</button>
      <button class="nav-item" data-sec="sd">SD Card</button>
      <button class="nav-item" data-sec="mqtt">MQTT Settings</button>
      <button class="nav-item" data-sec="system">System</button>
      <button class="nav-item" data-sec="password">Change Password</button>
    </nav>
    <div class="sb-foot">
      <button class="sb-logout" id="logoutBtn">Logout</button>
      <div class="sb-wifi" id="sbWifi">WiFi: -- &nbsp;|&nbsp; -- dBm</div>
      <div class="sb-univ">Invertis University, Bareilly</div>
    </div>
  </aside>

  <main class="main">
    <header class="topbar">
      <div class="page-title" id="pageTitle">Dashboard</div>
      <div class="top-right">
        <div class="spinner" id="refreshSpinner"></div>
        <div class="conn"><span class="dot" id="connDot"></span><span id="connTxt">Connecting</span></div>
        <div class="clock" id="clock">--:--:--</div>
      </div>
    </header>

    <div class="content">

      <!-- ===== Dashboard ===== -->
      <section class="section active" id="sec-dashboard">
        <h2>Overview</h2>
        <div class="sub">Live status of all conference room equipment</div>

        <div class="grid grid-4">
          <div class="card metric"><div class="lbl">Voltage</div><div class="val"><span id="mVolt">--</span><span class="unit">V</span></div></div>
          <div class="card metric"><div class="lbl">Main Current</div><div class="val"><span id="mCurr">--</span><span class="unit">A</span></div></div>
          <div class="card metric"><div class="lbl">Temperature</div><div class="val"><span id="mTemp">--</span><span class="unit">C</span></div></div>
          <div class="card metric"><div class="lbl">Humidity</div><div class="val"><span id="mHum">--</span><span class="unit">%</span></div></div>
        </div>

        <div style="height:18px"></div>

        <div class="grid grid-2">
          <div class="card slave-card">
            <div><div class="name">Digital Board</div><div class="info" id="dbInfo">RSSI: -- dBm</div></div>
            <span class="badge b-off" id="dbStatus">OFFLINE</span>
          </div>
          <div class="card slave-card">
            <div><div class="name">PZEM Sensor</div><div class="info" id="pzInfo">Health: --</div></div>
            <span class="badge b-off" id="pzStatus">OFFLINE</span>
          </div>
        </div>

        <div style="height:18px"></div>
        <h2 style="font-size:15px">Equipment Status</h2>
        <div class="pills" id="statusPills"></div>
      </section>

      <!-- ===== Lights & Relays ===== -->
      <section class="section" id="sec-lights">
        <h2>Lights and Relay Control</h2>
        <div class="sub">Toggle each relay individually or use the bulk actions below</div>
        <div class="relay-grid" id="relayGrid"></div>
        <div class="divider"></div>
        <div class="action-bar">
          <button class="btn btn-primary" id="allLightsOn">All Lights ON</button>
          <button class="btn btn-outline" id="allLightsOff">All Lights OFF</button>
          <div class="spacer"></div>
          <button class="btn btn-danger" id="roomShutdown">Room Shutdown</button>
        </div>
        <div class="note">Power Socket and Digital Board are not affected by the bulk light actions</div>
      </section>

      <!-- ===== Relay Locks ===== -->
      <section class="section" id="sec-locks">
        <h2>Relay Locks</h2>
        <div class="sub">A locked relay cannot be turned ON until unlocked</div>
        <div class="relay-grid" id="lockGrid"></div>
        <div class="divider"></div>
        <div class="action-bar">
          <button class="btn btn-outline" id="unlockAll">Unlock All</button>
        </div>
      </section>

      <!-- ===== AC Control ===== -->
      <section class="section" id="sec-ac">
        <h2>AC Control</h2>
        <div class="sub">Manage power, temperature, and fan speed</div>
        <div class="card ac-card">
          <div class="ac-row">
            <div class="lbl">Power</div>
            <div style="display:flex;align-items:center;gap:14px">
              <span id="acPwrTxt" class="relay-state off">OFF</span>
              <label class="toggle"><input type="checkbox" id="acPower"/><span class="track"></span></label>
            </div>
          </div>
          <div class="ac-row" style="flex-direction:column;align-items:stretch">
            <div style="display:flex;justify-content:space-between;align-items:center;width:100%">
              <div class="lbl">Temperature</div>
              <div class="temp-ctl">
                <button class="btn btn-gray" id="tempDown">Decrease</button>
                <div class="temp-val"><span id="acTempVal">--</span> C</div>
                <button class="btn btn-gray" id="tempUp">Increase</button>
              </div>
            </div>
            <div class="temp-exact">
              <label>Set Exact Temperature</label>
              <input type="number" min="16" max="30" id="acTempInput" placeholder="24"/>
              <button class="btn btn-navy" id="acTempSet">Set</button>
            </div>
          </div>
          <div class="ac-row" style="flex-direction:column;align-items:stretch">
            <div class="lbl" style="margin-bottom:10px">Fan Speed</div>
            <div class="seg" id="fanSeg">
              <button data-val="auto">AUTO</button>
              <button data-val="min">MIN</button>
              <button data-val="low">LOW</button>
              <button data-val="med">MED</button>
              <button data-val="high">HIGH</button>
              <button data-val="max">MAX</button>
            </div>
          </div>
        </div>
      </section>

      <!-- ===== Energy & Sensors ===== -->
      <section class="section" id="sec-energy">
        <h2>Energy and Sensors</h2>
        <div class="sub">Live energy consumption and environmental data</div>
        <h3 style="font-size:14px;font-weight:700;margin-bottom:10px;color:var(--muted);text-transform:uppercase;letter-spacing:0.5px">Energy</h3>
        <div class="grid grid-3">
          <div class="card metric"><div class="lbl">Main Energy</div><div class="val"><span id="eMain">--</span><span class="unit">kWh</span></div></div>
          <div class="card metric"><div class="lbl">AC Energy Today</div><div class="val"><span id="eAcToday">--</span><span class="unit">kWh</span></div></div>
          <div class="card metric"><div class="lbl">Digital Energy</div><div class="val"><span id="eDigital">--</span><span class="unit">kWh</span></div></div>
          <div class="card metric"><div class="lbl">AC Cumulative Energy</div><div class="val"><span id="eAcCum">--</span><span class="unit">kWh</span></div></div>
          <div class="card metric"><div class="lbl">AC Power</div><div class="val"><span id="eAcPower">--</span><span class="unit">W</span></div></div>
          <div class="card metric"><div class="lbl">AC Current</div><div class="val"><span id="eAcCurr">--</span><span class="unit">A</span></div></div>
          <div class="card metric"><div class="lbl">Energy Voltage</div><div class="val"><span id="eVolt">--</span><span class="unit">V</span></div></div>
        </div>
        <div style="height:20px"></div>
        <h3 style="font-size:14px;font-weight:700;margin-bottom:10px;color:var(--muted);text-transform:uppercase;letter-spacing:0.5px">Sensors</h3>
        <div class="grid grid-3">
          <div class="card metric">
            <div class="lbl">Temperature <span class="badge b-ok" id="dhtBadge" style="margin-left:6px">OK</span></div>
            <div class="val"><span id="sTemp">--</span><span class="unit">C</span></div>
          </div>
          <div class="card metric"><div class="lbl">Humidity</div><div class="val"><span id="sHum">--</span><span class="unit">%</span></div></div>
          <div class="card metric"><div class="lbl">ACS Current</div><div class="val"><span id="sAcs">--</span><span class="unit">A</span></div><div class="note" style="margin-top:6px">Main Load Current</div></div>
        </div>
      </section>

      <!-- ===== SD Card ===== -->
      <section class="section" id="sec-sd">
        <h2>SD Card</h2>
        <div class="sub">Storage status and energy log management</div>
        <div class="card" style="max-width:640px">
          <div class="sd-row"><span class="lbl">SD Status</span><span class="val" id="sdStat">--</span></div>
          <div class="sd-row"><span class="lbl">Total Storage</span><span class="val"><span id="sdTotal">--</span> MB</span></div>
          <div class="sd-row"><span class="lbl">Used Storage</span><span class="val"><span id="sdUsed">--</span> MB</span></div>
          <div class="sd-row"><span class="lbl">Free Storage</span><span class="val"><span id="sdFree">--</span> MB</span></div>
          <div class="bar">
            <div class="bar-track"><div class="bar-fill" id="sdBar" style="width:0%"></div></div>
            <div class="bar-pct" id="sdPct">0%</div>
          </div>
        </div>
        <div style="height:18px"></div>
        <div class="action-bar">
          <button class="btn btn-navy" id="sdLastBtn">Get Last Record</button>
          <button class="btn btn-danger" id="sdClearBtn">Clear Energy Logs</button>
        </div>
        <div class="mono-box" id="sdLastOut" style="display:none"></div>
      </section>

      <!-- ===== MQTT ===== -->
      <section class="section" id="sec-mqtt">
        <h2>MQTT Settings</h2>
        <div class="sub">Configure MQTT broker connection</div>
        <div style="margin-bottom:14px;font-size:13px">
          <strong>Status:</strong> <span id="mqttStatusTxt">--</span>
        </div>
        <div class="card" style="max-width:640px">
          <div class="form-row tg">
            <label>MQTT Enabled</label>
            <label class="toggle"><input type="checkbox" id="mqttEnabled"/><span class="track"></span></label>
          </div>
          <div class="form-row"><label>Broker Host</label><input type="text" id="mqttBroker" placeholder="broker.hivemq.com"/></div>
          <div class="form-row"><label>Port</label><input type="number" id="mqttPort" placeholder="1883"/></div>
          <div class="form-row"><label>Username</label><input type="text" id="mqttUser" placeholder="optional"/></div>
          <div class="form-row"><label>Password</label>
            <div class="pw-wrap">
              <input type="password" id="mqttPass"/>
              <button type="button" class="pw-toggle" data-target="mqttPass">Show</button>
            </div>
          </div>
          <div class="form-row"><label>Keepalive (seconds)</label><input type="number" id="mqttKeep" placeholder="60"/></div>
          <button class="btn btn-primary btn-full" id="mqttSave">Save Settings</button>
          <div style="height:10px"></div>
          <button class="btn btn-navy btn-full" id="mqttShowBtn">View Current Config</button>
          <div class="mono-box" id="mqttShowOut" style="display:none"></div>
        </div>
      </section>

      <!-- ===== System ===== -->
      <section class="section" id="sec-system">
        <h2>System</h2>
        <div class="sub">Device information and maintenance</div>
        <div class="card" style="max-width:640px">
          <div class="sd-row"><span class="lbl">Uptime</span><span class="val" id="sysUptime">--</span></div>
          <div class="sd-row"><span class="lbl">WiFi SSID</span><span class="val" id="sysSsid">--</span></div>
          <div class="sd-row"><span class="lbl">WiFi Signal</span><span class="val" id="sysRssi">--</span></div>
          <div class="sd-row"><span class="lbl">Current Time</span><span class="val" id="sysTime">--</span></div>
          <div class="sd-row"><span class="lbl">Time Source</span><span class="val" id="sysTimeSrc">--</span></div>
          <div class="sd-row"><span class="lbl">Reset Reason</span><span class="val" id="sysReset">--</span></div>
          <div class="sd-row"><span class="lbl">MQTT Status</span><span class="val" id="sysMqtt">--</span></div>
        </div>
        <div style="height:18px"></div>
        <div class="action-bar">
          <button class="btn btn-navy" id="sysRefresh">Refresh Status</button>
          <div class="spacer"></div>
          <button class="btn btn-danger" id="sysReboot">Reboot System</button>
        </div>
      </section>

      <!-- ===== Change Password ===== -->
      <section class="section" id="sec-password">
        <h2>Change Credentials</h2>
        <div class="sub">Update your username and password</div>
        <div class="card" style="max-width:480px;margin:0 auto">
          <form id="credForm" autocomplete="off">
            <div class="form-row">
              <label>Current Password</label>
              <div class="pw-wrap">
                <input type="password" id="curPass" required/>
                <button type="button" class="pw-toggle" data-target="curPass">Show</button>
              </div>
            </div>
            <div class="form-row"><label>New Username</label><input type="text" id="newUser" required/></div>
            <div class="form-row">
              <label>New Password</label>
              <div class="pw-wrap">
                <input type="password" id="newPass" required/>
                <button type="button" class="pw-toggle" data-target="newPass">Show</button>
              </div>
              <div class="err-msg" id="newPassErr" style="display:none;margin-top:6px;margin-bottom:0">Minimum 4 characters required</div>
            </div>
            <div class="form-row">
              <label>Confirm New Password</label>
              <input type="password" id="newPass2" required/>
              <div class="err-msg" id="newPass2Err" style="display:none;margin-top:6px;margin-bottom:0">Passwords do not match</div>
            </div>
            <button type="submit" class="btn btn-primary btn-full" id="credSubmit">Update Credentials</button>
          </form>
        </div>
      </section>

    </div>
  </main>
</div>

<!-- Global components -->
<div class="conn-banner" id="connBanner">Connection lost. Retrying...</div>
<div class="reboot-banner" id="rebootBanner">System is rebooting. Please wait...</div>
<div class="toasts" id="toasts"></div>
<div class="overlay" id="overlay">
  <div class="modal">
    <h3>Confirm Action</h3>
    <p id="modalMsg"></p>
    <input type="text" id="modalInput" placeholder="Type CONFIRM" style="display:none"/>
    <div class="modal-btns">
      <button class="btn btn-gray" id="modalCancel">Cancel</button>
      <button class="btn btn-danger" id="modalOk">Confirm</button>
    </div>
  </div>
</div>

<script>
// =================== Config ===================
const RELAY_LABELS = ["Light 1","Light 2","Light 3","Light 4","Light 5","Power Socket","Digital Board"];
const STATUS_PILL_LABELS = ["Light 1","Light 2","Light 3","Light 4","Light 5","Power Socket","Digital Board"];
const REFRESH_MS = 3000;

let statusData = null;
let pollTimer = null;
let connBad = false;

// =================== Utilities ===================
const $ = s => document.querySelector(s);
const $$ = s => document.querySelectorAll(s);

function fmtRuntime(sec){
  sec = Math.max(0, parseInt(sec||0));
  const h = String(Math.floor(sec/3600)).padStart(2,'0');
  const m = String(Math.floor((sec%3600)/60)).padStart(2,'0');
  const s = String(sec%60).padStart(2,'0');
  return `${h}:${m}:${s}`;
}
function fmtUptime(ms){
  return fmtRuntime(Math.floor((ms||0)/1000));
}
function num(v, d=1){
  if(v===undefined||v===null||isNaN(v)) return "--";
  return Number(v).toFixed(d);
}

async function api(path, opts={}){
  opts.credentials = "include";
  opts.headers = Object.assign({"Content-Type":"application/json"}, opts.headers||{});
  try{
    const ctl = new AbortController();
    const tid = setTimeout(()=>ctl.abort(), 8000);
    const res = await fetch(path, Object.assign({signal:ctl.signal}, opts));
    clearTimeout(tid);
    if(res.status===401){ window.location.href = "/login"; return null; }
    setConn(true);
    const ct = res.headers.get("content-type")||"";
    if(ct.includes("application/json")) return await res.json();
    return await res.text();
  }catch(e){
    setConn(false);
    throw e;
  }
}
function setConn(ok){
  const dot = $("#connDot"), txt = $("#connTxt"), bnr = $("#connBanner");
  if(!dot) return;
  if(ok){
    dot.className = "dot ok"; txt.textContent = "Connected"; bnr.classList.remove("show");
    connBad = false;
  }else{
    dot.className = "dot bad"; txt.textContent = "Offline"; bnr.classList.add("show");
    connBad = true;
  }
}

// =================== Toast ===================
function toast(msg, type="info"){
  const t = document.createElement("div");
  t.className = `toast ${type}`;
  t.textContent = msg;
  $("#toasts").appendChild(t);
  setTimeout(()=>{ t.style.opacity="0"; t.style.transition="opacity .3s"; setTimeout(()=>t.remove(),300); }, 2700);
}

// =================== Confirm modal ===================
function confirmDialog(message, requireType){
  return new Promise(resolve=>{
    const ov = $("#overlay"), inp = $("#modalInput"), ok = $("#modalOk");
    $("#modalMsg").textContent = message;
    if(requireType){
      inp.style.display="block"; inp.value=""; ok.disabled=true;
      inp.oninput = ()=>{ ok.disabled = inp.value.trim() !== requireType; };
    }else{
      inp.style.display="none"; ok.disabled=false;
    }
    ov.classList.add("show");
    const cleanup = (val)=>{ ov.classList.remove("show"); ok.onclick=null; $("#modalCancel").onclick=null; resolve(val); };
    ok.onclick = ()=>cleanup(true);
    $("#modalCancel").onclick = ()=>cleanup(false);
  });
}

// =================== Loading button ===================
async function withLoading(btn, fn){
  const orig = btn.textContent;
  btn.disabled = true;
  btn.innerHTML = '<span class="loading-dots"></span>';
  try{ return await fn(); }
  finally{ btn.disabled = false; btn.textContent = orig; }
}

// =================== Show/Hide password toggles ===================
document.addEventListener("click", e=>{
  if(e.target.classList.contains("pw-toggle")){
    const inp = document.getElementById(e.target.dataset.target);
    if(!inp) return;
    if(inp.type==="password"){ inp.type="text"; e.target.textContent="Hide"; }
    else{ inp.type="password"; e.target.textContent="Show"; }
  }
});

// =================== Login ===================
$("#loginForm").addEventListener("submit", async e=>{
  e.preventDefault();
  const err = $("#loginErr"); err.style.display="none";
  const btn = $("#loginBtn");
  await withLoading(btn, async ()=>{
    try{
      const r = await api("/api/login",{method:"POST",body:JSON.stringify({
        username:$("#loginUser").value, password:$("#loginPass").value
      })});
      if(r && r.ok){ showApp(); }
      else{ err.textContent = (r&&r.message)||"Invalid username or password"; err.style.display="block"; }
    }catch(_){
      err.textContent = "Could not connect to device"; err.style.display="block";
    }
  });
});

function showApp(){
  $("#loginPage").style.display="none";
  $("#appPage").style.display="flex";
  startPolling();
}
function showLogin(){
  if(pollTimer){ clearInterval(pollTimer); pollTimer=null; }
  $("#appPage").style.display="none";
  $("#loginPage").style.display="flex";
}

// =================== Logout ===================
$("#logoutBtn").addEventListener("click", async ()=>{
  try{ await api("/api/logout",{method:"POST"}); }catch(_){}
  showLogin();
});

// =================== Sidebar nav ===================
$$(".nav-item").forEach(n=>{
  n.addEventListener("click", ()=>{
    $$(".nav-item").forEach(x=>x.classList.remove("active"));
    n.classList.add("active");
    const sec = n.dataset.sec;
    $$(".section").forEach(s=>s.classList.remove("active"));
    $("#sec-"+sec).classList.add("active");
    $("#pageTitle").textContent = n.textContent;
    $("#sbNav").classList.remove("open");
    if(sec==="mqtt") loadMqtt();
  });
});
$("#sbToggle").addEventListener("click",()=>$("#sbNav").classList.toggle("open"));

// =================== Clock ===================
setInterval(()=>{
  const d = new Date();
  $("#clock").textContent =
    String(d.getHours()).padStart(2,'0')+":"+
    String(d.getMinutes()).padStart(2,'0')+":"+
    String(d.getSeconds()).padStart(2,'0');
},1000);

// =================== Build relay & lock cards ===================
function buildRelayCards(){
  const g = $("#relayGrid"); g.innerHTML = "";
  for(let i=0;i<7;i++){
    const c = document.createElement("div");
    c.className = "card relay-card";
    c.innerHTML = `
      <div class="relay-head">
        <div>
          <div class="relay-name">${RELAY_LABELS[i]}</div>
          <div class="relay-state off" id="rState${i}">OFF</div>
          <div class="relay-locked" id="rLock${i}" style="display:none">LOCKED</div>
        </div>
        <label class="toggle"><input type="checkbox" id="rTog${i}" data-relay="${i+1}"/><span class="track"></span></label>
      </div>
      <div class="relay-runtime">Runtime: <span id="rRt${i}">00:00:00</span></div>
    `;
    g.appendChild(c);
  }
  $$('input[id^="rTog"]').forEach(t=>{
    t.addEventListener("change", async e=>{
      const relay = parseInt(e.target.dataset.relay);
      const state = e.target.checked;
      try{
        const r = await api("/api/relay",{method:"POST",body:JSON.stringify({relay,state})});
        if(!r || !r.ok){
          toast((r&&r.message)||"Action failed","error");
          e.target.checked = !state;
        }else{
          toast(`${RELAY_LABELS[relay-1]} turned ${state?"ON":"OFF"}`,"success");
          fetchStatus();
        }
      }catch(_){
        toast("Connection error","error");
        e.target.checked = !state;
      }
    });
  });
}
function buildLockCards(){
  const g = $("#lockGrid"); g.innerHTML = "";
  for(let i=0;i<7;i++){
    const c = document.createElement("div");
    c.className = "card relay-card";
    c.innerHTML = `
      <div class="relay-head">
        <div>
          <div class="relay-name">${RELAY_LABELS[i]} Lock</div>
          <div class="relay-state off" id="lState${i}">UNLOCKED</div>
        </div>
        <label class="toggle lock"><input type="checkbox" id="lTog${i}" data-relay="${i+1}"/><span class="track"></span></label>
      </div>
    `;
    g.appendChild(c);
  }
  $$('input[id^="lTog"]').forEach(t=>{
    t.addEventListener("change", async e=>{
      const relay = parseInt(e.target.dataset.relay);
      const locked = e.target.checked;
      try{
        const r = await api("/api/lock",{method:"POST",body:JSON.stringify({relay,locked})});
        if(!r || !r.ok){
          toast((r&&r.message)||"Action failed","error");
          e.target.checked = !locked;
        }else{
          toast(`${RELAY_LABELS[relay-1]} ${locked?"locked":"unlocked"}`,"success");
          fetchStatus();
        }
      }catch(_){
        toast("Connection error","error");
        e.target.checked = !locked;
      }
    });
  });
}

// =================== Status pills ===================
function buildPills(){
  const c = $("#statusPills"); c.innerHTML="";
  STATUS_PILL_LABELS.forEach((lbl,i)=>{
    const p = document.createElement("div");
    p.className="pill";
    p.innerHTML = `
      <div class="pname">${lbl}</div>
      <div class="pstate off" id="pState${i}">OFF</div>
      <div class="plocked" id="pLocked${i}" style="display:none">LOCKED</div>
      <div class="pruntime" id="pRt${i}">00:00:00</div>
    `;
    c.appendChild(p);
  });
}

// =================== Render status ===================
function renderStatus(d){
  if(!d) return;
  statusData = d;

  // Dashboard metrics
  $("#mVolt").textContent = num(d.voltage,1);
  $("#mCurr").textContent = num(d.current,2);
  $("#mTemp").textContent = num(d.temperature,1);
  $("#mHum").textContent  = num(d.humidity,1);

  // Slaves
  const dbOn = !!d.digital_online;
  $("#dbStatus").textContent = dbOn?"ONLINE":"OFFLINE";
  $("#dbStatus").className = "badge "+(dbOn?"b-ok":"b-off");
  $("#dbInfo").textContent = "RSSI: "+(d.digital_rssi!==undefined?d.digital_rssi+" dBm":"-- dBm");

  const pzOn = !!d.pzem_online;
  $("#pzStatus").textContent = pzOn?"ONLINE":"OFFLINE";
  $("#pzStatus").className = "badge "+(pzOn?"b-ok":"b-off");
  $("#pzInfo").textContent = "Health: "+(d.pzem_ok?"OK":"FAIL");

  // Relays + pills + locks
  const relays = d.relays || [];
  const locks = d.locks || [];
  const runtimes = d.runtimes || [];
  for(let i=0;i<7;i++){
    const on = !!relays[i], lk = !!locks[i], rt = runtimes[i]||0;
    // Pills
    const ps = $("#pState"+i); if(ps){ ps.textContent = on?"ON":"OFF"; ps.className = "pstate "+(on?"on":"off"); }
    const pl = $("#pLocked"+i); if(pl) pl.style.display = lk?"block":"none";
    const pr = $("#pRt"+i); if(pr) pr.textContent = fmtRuntime(rt);
    // Relay cards
    const rt2 = $("#rTog"+i); if(rt2 && document.activeElement!==rt2) rt2.checked = on;
    const rs = $("#rState"+i); if(rs){ rs.textContent = on?"ON":"OFF"; rs.className = "relay-state "+(on?"on":"off"); }
    const rl = $("#rLock"+i); if(rl) rl.style.display = lk?"inline-block":"none";
    const rrt = $("#rRt"+i); if(rrt) rrt.textContent = fmtRuntime(rt);
    // Locks
    const lt = $("#lTog"+i); if(lt && document.activeElement!==lt) lt.checked = lk;
    const ls = $("#lState"+i); if(ls){ ls.textContent = lk?"LOCKED":"UNLOCKED"; ls.style.color = lk?"var(--accent)":"var(--muted)"; ls.style.fontWeight="700"; }
  }

  // Energy
  $("#eMain").textContent = num(d.energy_main,3);
  $("#eAcToday").textContent = num(d.ac_energy_today,3);
  $("#eDigital").textContent = num(d.energy_digital,3);
  $("#eAcCum").textContent = num(d.ac_energy_cum,3);
  $("#eAcPower").textContent = num(d.ac_power,0);
  $("#eAcCurr").textContent = num(d.ac_current,2);
  $("#eVolt").textContent = num(d.voltage,1);
  $("#sTemp").textContent = num(d.temperature,1);
  $("#sHum").textContent = num(d.humidity,1);
  $("#sAcs").textContent = num(d.acs_current,2);
  const dhtOk = d.dht_ok !== false;
  const db = $("#dhtBadge"); db.textContent = dhtOk?"OK":"ERROR"; db.className = "badge "+(dhtOk?"b-ok":"b-err");

  // SD
  if(d.sd){
    $("#sdStat").textContent = d.sd.ok?"OK":"ERROR";
    $("#sdStat").style.color = d.sd.ok?"var(--success)":"var(--danger)";
    $("#sdTotal").textContent = num(d.sd.total_mb,0);
    $("#sdUsed").textContent  = num(d.sd.used_mb,0);
    $("#sdFree").textContent  = num(d.sd.free_mb,0);
    const pct = d.sd.total_mb>0 ? Math.min(100, Math.round(d.sd.used_mb/d.sd.total_mb*100)) : 0;
    $("#sdBar").style.width = pct+"%";
    $("#sdPct").textContent = pct+"%";
  }

  // AC
  if(d.ac){
    const acOn = !!d.ac.power;
    const acT = $("#acPower"); if(acT && document.activeElement!==acT) acT.checked = acOn;
    $("#acPwrTxt").textContent = acOn?"ON":"OFF";
    $("#acPwrTxt").className = "relay-state "+(acOn?"on":"off");
    $("#acTempVal").textContent = d.ac.temp!==undefined?d.ac.temp:"--";
    $$("#fanSeg button").forEach(b=>{
      b.classList.toggle("active", b.dataset.val===(d.ac.fan||"").toLowerCase());
    });
  }

  // MQTT
  const mst = d.mqtt_status;
  const mTxt = mst===2?"Connected":mst===1?"Connecting":mst===3?"Failed":"Disabled";
  const mColor = mst===2?"var(--success)":mst===3?"var(--danger)":"var(--muted)";
  const mEl = $("#mqttStatusTxt"); mEl.textContent = mTxt; mEl.style.color = mColor; mEl.style.fontWeight="700";
  $("#sysMqtt").textContent = `${mst}  (${mTxt})`;

  // System
  $("#sysUptime").textContent = fmtUptime(d.uptime_ms||d.uptime||0);
  $("#sysSsid").textContent = d.wifi_ssid||"--";
  const rssi = d.wifi_rssi;
  let sigDesc = "--";
  if(rssi!==undefined){
    if(rssi > -60) sigDesc = "Excellent";
    else if(rssi >= -70) sigDesc = "Good";
    else sigDesc = "Weak";
    $("#sysRssi").textContent = `${rssi} dBm (${sigDesc})`;
    $("#sbWifi").textContent = `WiFi: ${d.wifi_ssid||"--"}  |  ${rssi} dBm`;
  }
  $("#sysTime").textContent = d.time||"--";
  $("#sysTimeSrc").textContent = d.time_source||"--";
  $("#sysReset").textContent = d.reset_reason||"--";
}

async function fetchStatus(showSpinner){
  if(showSpinner) $("#refreshSpinner").classList.add("show");
  try{
    const d = await api("/api/status");
    renderStatus(d);
  }catch(_){}
  finally{ if(showSpinner) $("#refreshSpinner").classList.remove("show"); }
}
function startPolling(){
  fetchStatus(true);
  if(pollTimer) clearInterval(pollTimer);
  pollTimer = setInterval(fetchStatus, REFRESH_MS);
}

// =================== Bulk actions ===================
$("#allLightsOn").addEventListener("click", async e=>{
  await withLoading(e.target, async ()=>{
    try{ const r = await api("/api/lights",{method:"POST",body:JSON.stringify({state:true})});
      if(r&&r.ok){ toast("All lights turned ON","success"); fetchStatus(); }
      else toast("Action failed","error");
    }catch(_){ toast("Connection error","error"); }
  });
});
$("#allLightsOff").addEventListener("click", async e=>{
  await withLoading(e.target, async ()=>{
    try{ const r = await api("/api/lights",{method:"POST",body:JSON.stringify({state:false})});
      if(r&&r.ok){ toast("All lights turned OFF","success"); fetchStatus(); }
      else toast("Action failed","error");
    }catch(_){ toast("Connection error","error"); }
  });
});
$("#roomShutdown").addEventListener("click", async e=>{
  const ok = await confirmDialog("This will turn OFF all conference room equipment including all lights, power socket, and digital board. Are you sure?");
  if(!ok) return;
  await withLoading(e.target, async ()=>{
    try{ const r = await api("/api/shutdown",{method:"POST"});
      if(r&&r.ok){ toast("Room shutdown complete","success"); fetchStatus(); }
      else toast("Shutdown failed","error");
    }catch(_){ toast("Connection error","error"); }
  });
});
$("#unlockAll").addEventListener("click", async e=>{
  const ok = await confirmDialog("This will remove all relay locks. Continue?");
  if(!ok) return;
  await withLoading(e.target, async ()=>{
    try{ const r = await api("/api/unlock-all",{method:"POST"});
      if(r&&r.ok){ toast("All locks removed","success"); fetchStatus(); }
      else toast("Action failed","error");
    }catch(_){ toast("Connection error","error"); }
  });
});

// =================== AC controls ===================
$("#acPower").addEventListener("change", async e=>{
  const v = e.target.checked?"on":"off";
  try{ const r = await api("/api/ac",{method:"POST",body:JSON.stringify({cmd:"power",val:v})});
    if(r&&r.ok){ toast("AC powered "+v.toUpperCase(),"success"); fetchStatus(); }
    else{ toast((r&&r.message)||"AC command failed","error"); e.target.checked=!e.target.checked; }
  }catch(_){ toast("Connection error","error"); e.target.checked=!e.target.checked; }
});
async function tempStep(dir){
  try{ const r = await api("/api/ac",{method:"POST",body:JSON.stringify({cmd:"temp_step",val:dir})});
    if(r&&r.ok){ fetchStatus(); }else toast((r&&r.message)||"Failed","error");
  }catch(_){ toast("Connection error","error"); }
}
$("#tempUp").addEventListener("click",()=>tempStep("up"));
$("#tempDown").addEventListener("click",()=>tempStep("down"));
$("#acTempSet").addEventListener("click", async e=>{
  const v = parseInt($("#acTempInput").value);
  if(isNaN(v)||v<16||v>30){ toast("Enter a value between 16 and 30","error"); return; }
  await withLoading(e.target, async ()=>{
    try{ const r = await api("/api/ac",{method:"POST",body:JSON.stringify({cmd:"temp",val:v})});
      if(r&&r.ok){ toast(`Temperature set to ${v} C`,"success"); fetchStatus(); }
      else toast((r&&r.message)||"Failed","error");
    }catch(_){ toast("Connection error","error"); }
  });
});
$$("#fanSeg button").forEach(b=>{
  b.addEventListener("click", async ()=>{
    const val = b.dataset.val;
    try{ const r = await api("/api/ac",{method:"POST",body:JSON.stringify({cmd:"fan",val})});
      if(r&&r.ok){ toast(`Fan speed: ${val.toUpperCase()}`,"success"); fetchStatus(); }
      else toast((r&&r.message)||"Failed","error");
    }catch(_){ toast("Connection error","error"); }
  });
});

// =================== SD ===================
$("#sdLastBtn").addEventListener("click", async e=>{
  await withLoading(e.target, async ()=>{
    try{
      const r = await api("/api/sd-last-record");
      if(r && r.ok){
        const out = $("#sdLastOut");
        out.style.display="block";
        out.textContent = `Record ID: ${r.record_id}\nDate:      ${r.date}`;
      }else toast((r&&r.message)||"No record found","error");
    }catch(_){ toast("Connection error","error"); }
  });
});
$("#sdClearBtn").addEventListener("click", async ()=>{
  const ok = await confirmDialog("This will permanently delete all stored energy logs from the SD card. Type CONFIRM to proceed.","CONFIRM");
  if(!ok) return;
  try{
    const r = await api("/api/clear-logs",{method:"POST",body:JSON.stringify({confirm:"CONFIRM"})});
    if(r&&r.ok) toast("Energy logs cleared","success");
    else toast((r&&r.message)||"Clear failed","error");
  }catch(_){ toast("Connection error","error"); }
});

// =================== MQTT ===================
async function loadMqtt(){
  try{
    const r = await api("/api/mqtt-show");
    if(r){
      $("#mqttEnabled").checked = !!r.enabled;
      $("#mqttBroker").value = r.broker||"";
      $("#mqttPort").value = r.port||"";
      $("#mqttUser").value = r.username||"";
      $("#mqttKeep").value = r.keepalive||"";
    }
  }catch(_){}
}
$("#mqttSave").addEventListener("click", async e=>{
  await withLoading(e.target, async ()=>{
    const body = {
      enabled: $("#mqttEnabled").checked,
      broker: $("#mqttBroker").value.trim(),
      port: parseInt($("#mqttPort").value)||1883,
      username: $("#mqttUser").value.trim(),
      password: $("#mqttPass").value,
      keepalive: parseInt($("#mqttKeep").value)||60
    };
    try{
      const r = await api("/api/mqtt",{method:"POST",body:JSON.stringify(body)});
      if(r&&r.ok) toast("MQTT settings saved","success");
      else toast((r&&r.message)||"Save failed","error");
    }catch(_){ toast("Connection error","error"); }
  });
});
$("#mqttShowBtn").addEventListener("click", async e=>{
  await withLoading(e.target, async ()=>{
    try{
      const r = await api("/api/mqtt-show");
      const out = $("#mqttShowOut");
      if(r){
        out.style.display="block";
        out.textContent =
`Enabled:   ${r.enabled}
Broker:    ${r.broker||"--"}
Port:      ${r.port||"--"}
Username:  ${r.username||"--"}
Topic:     ${r.topic||"--"}
Keepalive: ${r.keepalive||"--"} s`;
      }
    }catch(_){ toast("Connection error","error"); }
  });
});

// =================== System ===================
$("#sysRefresh").addEventListener("click", async e=>{
  await withLoading(e.target, async ()=>{
    try{
      const r = await api("/api/wifi-status");
      if(r){
        $("#sysSsid").textContent = r.ssid||"--";
        const rssi = r.rssi;
        let desc="--";
        if(rssi!==undefined){
          desc = rssi>-60?"Excellent":rssi>=-70?"Good":"Weak";
          $("#sysRssi").textContent = `${rssi} dBm (${desc})`;
        }
        toast("Status refreshed","success");
      }
      fetchStatus(true);
    }catch(_){ toast("Connection error","error"); }
  });
});
$("#sysReboot").addEventListener("click", async ()=>{
  const ok = await confirmDialog("This will reboot the ESP32 master and slave devices. The dashboard will be unavailable for approximately 10 seconds. Continue?");
  if(!ok) return;
  try{
    await api("/api/reboot",{method:"POST"});
  }catch(_){}
  $("#rebootBanner").classList.add("show");
  setTimeout(()=>window.location.reload(), 12000);
});

// =================== Change credentials ===================
$("#credForm").addEventListener("submit", async e=>{
  e.preventDefault();
  const cp=$("#curPass").value, nu=$("#newUser").value.trim(), np=$("#newPass").value, np2=$("#newPass2").value;
  $("#newPassErr").style.display = np.length<4?"block":"none";
  $("#newPass2Err").style.display = np!==np2?"block":"none";
  if(np.length<4 || np!==np2) return;
  const btn = $("#credSubmit");
  await withLoading(btn, async ()=>{
    try{
      const r = await api("/api/change-credentials",{method:"POST",body:JSON.stringify({current_pass:cp,new_user:nu,new_pass:np})});
      if(r&&r.ok){
        toast("Credentials updated. Logging out...","success");
        setTimeout(async ()=>{ try{await api("/api/logout",{method:"POST"});}catch(_){}; showLogin(); }, 3000);
      }else{
        toast((r&&r.message)||"Update failed","error");
      }
    }catch(_){ toast("Connection error","error"); }
  });
});

// =================== Boot ===================
buildRelayCards();
buildLockCards();
buildPills();

// Try to detect existing session: hit /api/status; if 200, show app; if 401, login
(async ()=>{
  try{
    const r = await fetch("/api/status",{credentials:"include"});
    if(r.status===401){ showLogin(); return; }
    if(r.ok){ const d = await r.json(); showApp(); renderStatus(d); }
    else showLogin();
  }catch(_){ showLogin(); }
})();
</script>
</body>
</html>

)=====";
