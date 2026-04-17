const SERVER = 'http://localhost:8080';
let userToken = '';
let userRole = 'civilian';

// DOM Elements
const loginOverlay = document.getElementById('loginOverlay');
const appContainer = document.getElementById('appContainer');
const loginBtn = document.getElementById('loginBtn');
const usernameInput = document.getElementById('usernameInput');
const loginStatus = document.getElementById('loginStatus');
const roleText = document.getElementById('roleText');
const terminalLog = document.getElementById('terminalLog');

// Metrics
let totalCars = 0;
let totalVotes = 0;
let totalOverrides = 0;

// Canvas
const canvas = document.getElementById('trafficCanvas');
const ctx = canvas.getContext('2d');
let cw, ch;
const LANE_WIDTH = 30;

// 4-Junction Network State
let junctions = [
    { id: 'NW', state: 'NS', timer: 0, override: false, cx: 0, cy: 0 },
    { id: 'NE', state: 'NS', timer: 100, override: false, cx: 0, cy: 0 },
    { id: 'SW', state: 'EW', timer: 50, override: false, cx: 0, cy: 0 },
    { id: 'SE', state: 'EW', timer: 150, override: false, cx: 0, cy: 0 }
];
const LIGHT_DURATION = 300; 

function resize() {
  const rect = canvas.parentElement.getBoundingClientRect();
  canvas.width = rect.width;
  canvas.height = rect.height;
  cw = canvas.width;
  ch = canvas.height;
  
  junctions[0].cx = cw/3; junctions[0].cy = ch/3;
  junctions[1].cx = cw*2/3; junctions[1].cy = ch/3;
  junctions[2].cx = cw/3; junctions[2].cy = ch*2/3;
  junctions[3].cx = cw*2/3; junctions[3].cy = ch*2/3;
}
window.addEventListener('resize', resize);

// === AUTHENTICATION ===
loginBtn.addEventListener('click', async () => {
  const user = usernameInput.value.trim().toLowerCase();
  if (!user) return;

  loginBtn.textContent = "Negotiating...";
  try {
    const res = await fetch(SERVER + '/login', {
      method: 'POST',
      body: JSON.stringify({ username: user })
    });
    if (!res.ok) throw new Error("Server rejected connection");
    
    const data = await res.json();
    userToken = data.token;
    userRole = data.user;
    
    roleText.textContent = userRole.toUpperCase();
    if (userRole === 'ambulance' || userRole === 'admin' || userRole === 'vip') {
      document.getElementById('userBadge').style.borderColor = "var(--primary)";
      document.getElementById('userBadge').style.color = "var(--primary)";
    }

    loginOverlay.classList.remove('active');
    appContainer.classList.remove('hidden');
    
    resize();
    logTerminal(`Auth successful. Token acquired. Role: ${userRole.toUpperCase()}`, true);
    requestAnimationFrame(renderLoop);
    
  } catch (err) {
    loginStatus.textContent = "Connection failed. Is the C++ server running on port 8080?";
    loginBtn.textContent = "Request Token";
  }
});

usernameInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') loginBtn.click();
});

// === SERVER COMMUNICATION ===
function logTerminal(msg, isTx = false, isOverride = false) {
  const d = new Date();
  const time = d.toTimeString().split(' ')[0] + '.' + String(d.getMilliseconds()).padStart(3, '0');
  const el = document.createElement('div');
  el.className = `log-entry ${isTx ? 'tx' : 'rx'}`;
  const pM = isTx ? 'POST' : 'RCV';
  el.innerHTML = `<span class="time">[${time}]</span><span class="${isOverride ? 'cmd text-red' : 'cmd'}">${pM}</span> ${msg}`;
  terminalLog.appendChild(el);
  terminalLog.scrollTop = terminalLog.scrollHeight;
  if(terminalLog.children.length > 50) terminalLog.removeChild(terminalLog.firstChild);
}

// Global Sync Mechanism
let lastEventId = -1;

async function broadcastEvent(eventData) {
    try {
        logTerminal(`/api/events - ${JSON.stringify(eventData.action)}`, true);
        await fetch(SERVER + '/api/events', {
            method: 'POST',
            body: JSON.stringify(eventData)
        });
    } catch(e) {}
}

async function pollGlobalEvents() {
    try {
        const res = await fetch(`${SERVER}/api/events?since=${lastEventId}`);
        const events = await res.json();
        for (let ev of events) {
            if (ev.id > lastEventId) lastEventId = ev.id;
            handleGlobalEvent(ev.payload);
        }
    } catch(e) {}
    setTimeout(pollGlobalEvents, 500);
}

function handleGlobalEvent(payload) {
    try {
        // Handle case where body is sent as string or object
        if (typeof payload === 'string') payload = JSON.parse(payload);
    } catch(e) { return; }

    if (payload.action === 'SPAWN_AMBULANCE') {
        vehicles.push(new Vehicle('ambulance'));
        logTerminal(`[SYNC] Ambulance Dispatched`, false);
    }
    else if (payload.action === 'SPAWN_VIP') {
        let pStart = payload.start || 'NW';
        let pEnd = payload.end || 'SE';
        activeVipRoute = runDijkstra(pStart, pEnd);
        let vip = new Vehicle('vip');
        
        if (pStart === 'NW') { vip.x = cw/3 - 15; vip.y = -50; vip.dir = 0; }
        else if (pStart === 'SW') { vip.x = -50; vip.y = ch*2/3 + 15; vip.dir = 3; }
        else if (pStart === 'SE') { vip.x = cw*2/3 + 15; vip.y = ch + 50; vip.dir = 2; }
        else if (pStart === 'NE') { vip.x = cw + 50; vip.y = ch/3 - 15; vip.dir = 1; }
        
        vip.route = [...activeVipRoute];
        if (pEnd === 'NW') vip.vipExitDir = 2; // North
        else if (pEnd === 'SW') vip.vipExitDir = 1; // West
        else if (pEnd === 'SE') vip.vipExitDir = 0; // South
        else if (pEnd === 'NE') vip.vipExitDir = 3; // East

        vehicles.push(vip);
        logTerminal(`[SYNC] VIP Routing: ${pStart} -> ${pEnd}`, false);
    }
    else if (payload.action === 'VIP_HACK_OVERRIDE') {
        let j = junctions.find(jx => jx.id === payload.j_id);
        if (j) {
            j.state = payload.state;
            j.override = true;
            j.timer = 0; // Reset standard timer to extend
            logTerminal(`[SYNC] VIP Hacker Override -> ${j.id}`, false, true);
        }
    }
    else if (payload.action === 'INJECT_TRAFFIC_BURST') {
        for (let i=0; i<6; i++) {
            setTimeout(() => { vehicles.push(new Vehicle('civilian', payload.dir)); }, i * 400);
        }
        logTerminal(`[SYNC] Traffic Surge Injected`, false);
    }
}

// Start polling immediately
pollGlobalEvents();

// === ENTITIES ===
const vehicles = [];
const COLORS = ['#ffe234', '#00e5ff', '#00ff88', '#ffffff'];

class Vehicle {
  constructor(type = 'civilian', presetDir = null) {
    this.type = type;
    this.dir = presetDir !== null ? presetDir : Math.floor(Math.random() * 4); 
    // 0:S, 1:W, 2:N, 3:E
    this.speed = type === 'ambulance' ? 3.5 : (type === 'vip' ? 2.5 : 1 + Math.random());
    this.color = type === 'ambulance' ? '#ff3b55' : (type === 'vip' ? '#ff00ff' : COLORS[Math.floor(Math.random() * COLORS.length)]);
    this.w = (this.type === 'ambulance' || this.type === 'vip') ? 12 : 10;
    this.h = (this.type === 'ambulance' || this.type === 'vip') ? 22 : 18;
    
    this.route = []; // Used by VIP for pathfinding
    
    // Pick side
    let roadIdx = Math.floor(Math.random() * 2);
    
    if (this.dir === 0) { // Southbound
       this.x = roadIdx === 0 ? cw/3 - 15 : cw*2/3 - 15;
       this.y = -50;
    }
    if (this.dir === 2) { // Northbound
       this.x = roadIdx === 0 ? cw/3 + 15 : cw*2/3 + 15;
       this.y = ch + 50;
    }
    if (this.dir === 1) { // Westbound (moving left)
       this.w = this.h; this.h = 10;
       this.x = cw + 50;
       this.y = roadIdx === 0 ? ch/3 - 15 : ch*2/3 - 15;
    }
    if (this.dir === 3) { // Eastbound (moving right)
       this.w = this.h; this.h = 10;
       this.x = -50;
       this.y = roadIdx === 0 ? ch/3 + 15 : ch*2/3 + 15;
    }
  }

  update() {
    let moving = true;
    const pad = 40; // Stopping distance
    
    // Pathfinding / Navigation Engine (for VIPs turning)
    if (this.route && this.route.length > 0) {
        let nextNodeId = this.route[0];
        let nextJ = junctions.find(j => j.id === nextNodeId);
        if (nextJ) {
            let dx = this.x - nextJ.cx;
            let dy = this.y - nextJ.cy;
            
            // Reached center of junction? Trigger navigation turn
            if (Math.abs(dx) < 20 && Math.abs(dy) < 20) {
                this.route.shift(); // Clear this waypoint
                
                if (this.route.length > 0) {
                    let targetJ = junctions.find(j => j.id === this.route[0]);
                    if (targetJ) {
                        // Change Direction
                        if (targetJ.cx - nextJ.cx > 10) this.dir = 3; // Turn East
                        else if (nextJ.cx - targetJ.cx > 10) this.dir = 1; // Turn West
                        else if (targetJ.cy - nextJ.cy > 10) this.dir = 0; // Turn South
                        else if (nextJ.cy - targetJ.cy > 10) this.dir = 2; // Turn North
                        
                        // Snap bounds and rotate hitbox
                        this.w = (this.dir === 1 || this.dir === 3) ? 22 : 12;
                        this.h = (this.dir === 1 || this.dir === 3) ? 12 : 22;
                        
                        if (this.dir === 0) this.x = nextJ.cx - 15;
                        if (this.dir === 2) this.x = nextJ.cx + 15;
                        if (this.dir === 1) this.y = nextJ.cy - 15;
                        if (this.dir === 3) this.y = nextJ.cy + 15;
                    }
                } else {
                    // Reached final junction loop bounds, proceed out to specific exit pseudo-lane
                    this.dir = this.vipExitDir !== undefined ? this.vipExitDir : 0; 
                    this.w = (this.dir === 1 || this.dir === 3) ? 22 : 12; 
                    this.h = (this.dir === 1 || this.dir === 3) ? 12 : 22;
                    
                    if (this.dir === 0) this.x = nextJ.cx - 15;
                    if (this.dir === 2) this.x = nextJ.cx + 15;
                    if (this.dir === 1) this.y = nextJ.cy - 15;
                    if (this.dir === 3) this.y = nextJ.cy + 15;
                }
            }
        }
    }
    
    // Auto-stopping at Lights
    for (let j of junctions) {
        let dx = this.x - j.cx;
        let dy = this.y - j.cy;
        
        if (this.dir === 0 || this.dir === 2) {
           if (Math.abs(dx) < 30) {
               if (j.state !== 'NS') { 
                   if (this.dir === 0 && this.y > j.cy - 70 && this.y < j.cy - pad) moving = false;
                   if (this.dir === 2 && this.y < j.cy + 70 && this.y > j.cy + pad) moving = false;
               }
           }
        } else {
           if (Math.abs(dy) < 30) {
               if (j.state !== 'EW') {
                   if (this.dir === 1 && this.x < j.cx + 70 && this.x > j.cx + pad) moving = false;
                   if (this.dir === 3 && this.x > j.cx - 70 && this.x < j.cx - pad) moving = false;
               }
           }
        }
    }
    
    // Collision checking
    for (let v of vehicles) {
      if (v !== this && v.dir === this.dir && Math.abs(this.x - v.x) < 5 && Math.abs(this.y - v.y) < 5) {
         if (this.dir === 0 && this.y < v.y && this.y > v.y - 30) moving = false;
         if (this.dir === 2 && this.y > v.y && this.y < v.y + 30) moving = false;
         if (this.dir === 1 && this.x > v.x && this.x < v.x + 30) moving = false;
         if (this.dir === 3 && this.x < v.x && this.x > v.x - 30) moving = false;
      }
    }

    if (moving) {
      if (this.dir === 0) this.y += this.speed;
      if (this.dir === 1) this.x -= this.speed;
      if (this.dir === 2) this.y -= this.speed;
      if (this.dir === 3) this.x += this.speed;
    }
  }

  draw(ctx) {
    ctx.shadowBlur = (this.type === 'ambulance' || this.type === 'vip') ? 15 : 5;
    ctx.shadowColor = this.color;
    ctx.fillStyle = this.color;
    ctx.fillRect(this.x - this.w/2, this.y - this.h/2, this.w, this.h);
    
    if (this.type === 'ambulance') {
        ctx.fillStyle = (Date.now() % 300 < 150) ? '#0077ff' : '#ffffff';
        ctx.fillRect(this.x - this.w/2, this.y - this.h/2, this.w, 4);
    }
    if (this.type === 'vip') {
        ctx.fillStyle = '#ffffff';
         ctx.fillRect(this.x - this.w/4, this.y - this.h/4, this.w/2, this.h/2);
    }
    ctx.shadowBlur = 0;
  }
}

// === DIJKSTRA'S ALGORITHM ENGINE ===
let activeVipRoute = null;

function getEdgeWeight(junctionA, junctionB) {
    let count = 1; // Base cost
    // Count exact vehicles physically residing in the bounding box between J1 and J2
    for (let v of vehicles) {
        if (junctionA === 'NW' && junctionB === 'SW' || junctionA === 'SW' && junctionB === 'NW') {
            if (v.x < cw/2 && v.y > ch/3 && v.y < ch*2/3) count++;
        }
        else if (junctionA === 'NW' && junctionB === 'NE' || junctionA === 'NE' && junctionB === 'NW') {
            if (v.y < ch/2 && v.x > cw/3 && v.x < cw*2/3) count++;
        }
        else if (junctionA === 'SW' && junctionB === 'SE' || junctionA === 'SE' && junctionB === 'SW') {
            if (v.y > ch/2 && v.x > cw/3 && v.x < cw*2/3) count++;
        }
        else if (junctionA === 'NE' && junctionB === 'SE' || junctionA === 'SE' && junctionB === 'NE') {
            if (v.x > cw/2 && v.y > ch/3 && v.y < ch*2/3) count++;
        }
    }
    return count;
}

function runDijkstra(startNode, targetNode) {
    // Dynamic Adjacency List based on live vehicle tracking!
    const graph = {
        'NW': { 'SW': getEdgeWeight('NW','SW'), 'NE': getEdgeWeight('NW','NE') },
        'NE': { 'SE': getEdgeWeight('NE','SE'), 'NW': getEdgeWeight('NW','NE') },
        'SW': { 'SE': getEdgeWeight('SW','SE'), 'NW': getEdgeWeight('NW','SW') },
        'SE': { 'NE': getEdgeWeight('NE','SE'), 'SW': getEdgeWeight('SW','SE') }
    };
    
    // Standard Dijkstra Implementation
    const costs = { 'NW': Infinity, 'NE': Infinity, 'SW': Infinity, 'SE': Infinity };
    const parents = { 'NW': null, 'NE': null, 'SW': null, 'SE': null };
    const processed = [];
    
    costs[startNode] = 0;
    let node = startNode;
    
    while (node !== null) {
        let cost = costs[node];
        let neighbors = graph[node];
        for (let n in neighbors) {
            let newCost = cost + neighbors[n];
            if (newCost < costs[n]) {
                costs[n] = newCost;
                parents[n] = node;
            }
        }
        processed.push(node);
        
        let lowest = Infinity;
        let lowestNode = null;
        for (let un in costs) {
            if (!processed.includes(un) && costs[un] < lowest) {
                lowest = costs[un];
                lowestNode = un;
            }
        }
        node = lowestNode;
    }
    
    let optimalPath = [targetNode];
    let parent = parents[targetNode];
    while (parent) {
        optimalPath.unshift(parent);
        parent = parents[parent];
    }
    
    logTerminal(`Dijkstra Route Computed: ${optimalPath.join(' -> ')}`, false);
    return optimalPath;
}

// === INTERACTIVITY ===
document.querySelectorAll('.lane-btn').forEach(btn => {
  btn.addEventListener('click', () => {
     if(btn.id === 'vipOverrideBtn') return; // skip
     let dir = parseInt(btn.dataset.road);
     broadcastEvent({ action: "INJECT_TRAFFIC_BURST", dir: dir });
  });
});

document.getElementById('spawnAmbulanceBtn').addEventListener('click', () => {
    broadcastEvent({ action: "SPAWN_AMBULANCE" });
});

document.getElementById('spawnVipBtn').addEventListener('click', () => {
    let nodes = ['NW','NE','SW','SE'];
    let start = nodes[Math.floor(Math.random()*4)];
    let end = nodes[Math.floor(Math.random()*4)];
    while(start === end) end = nodes[Math.floor(Math.random()*4)];
    broadcastEvent({ action: "SPAWN_VIP", start: start, end: end });
});

document.getElementById('vipOverrideBtn').addEventListener('click', () => {
    let vip = vehicles.find(v => v.type === 'vip');
    if (vip) {
        let closestJ = null; let minDist = Infinity;
        for (let j of junctions) {
            let d = Math.abs(vip.x - j.cx) + Math.abs(vip.y - j.cy);
            if (d < minDist && d < 180) { minDist = d; closestJ = j; }
        }
        if (closestJ) {
            let pState = (vip.dir === 0 || vip.dir === 2) ? 'NS' : 'EW';
            broadcastEvent({ action: "VIP_HACK_OVERRIDE", j_id: closestJ.id, state: pState });
        }
    } else {
        alert("No VIP vehicle detected on the grid.");
    }
});

// === RENDERING ENGINE ===
function renderLoop() {
  
  // Distributed Autonomous Sensing
  junctions.forEach(j => {
      j.override = false;
      let closestAmbulance = null;
      let minDist = Infinity;
      
      // Green Wave proximity check
      for (let v of vehicles) {
          if (v.type === 'ambulance') {
              let dx = v.x - j.cx;
              let dy = v.y - j.cy;
              let dist = Infinity;
              if ((v.dir === 0 || v.dir === 2) && Math.abs(dx) < 30) {
                  if (v.dir === 0) dist = j.cy - v.y; 
                  if (v.dir === 2) dist = v.y - j.cy;
              } else if ((v.dir === 1 || v.dir === 3) && Math.abs(dy) < 30) {
                  if (v.dir === 1) dist = v.x - j.cx;
                  if (v.dir === 3) dist = j.cx - v.x;
              }
              if (dist > -70 && dist < 300) {
                  if (dist < minDist) { minDist = dist; closestAmbulance = v; }
              }
          }
      }
      
      if (closestAmbulance) {
          j.override = true;
          let pState = (closestAmbulance.dir === 0 || closestAmbulance.dir === 2) ? 'NS' : 'EW';
          if (j.state !== pState) {
              j.state = pState;
              // We do not broadcast ambient Green Waves. They auto-compute on all clients simultaneously because the Ambulance object is perfectly synchronized!
          }
      }
      
      // Standard local timer
      if (!j.override) {
          j.timer++;
          if (j.timer > LIGHT_DURATION) {
              j.state = j.state === 'NS' ? 'EW' : 'NS';
              j.timer = 0;
          }
      }
  });

  // Random Spawns
  if (Math.random() < 0.03) vehicles.push(new Vehicle());
  
  // Clean off-screen & Active Route logic
  let vipAlive = false;
  for (let i = vehicles.length - 1; i >= 0; i--) {
    let v = vehicles[i];
    v.update();
    if (v.type === 'vip') vipAlive = true;
    if (v.x < -100 || v.x > cw+100 || v.y < -100 || v.y > ch+100) {
      vehicles.splice(i, 1);
    }
  }
  if (!vipAlive) activeVipRoute = null; // Remove GPS trail when VIP exits
  
  document.getElementById('mCars').textContent = vehicles.length;
  
  // Draw World
  ctx.clearRect(0, 0, cw, ch);
  
  ctx.fillStyle = '#111721';
  ctx.fillRect(cw/3 - LANE_WIDTH, 0, LANE_WIDTH*2, ch);
  ctx.fillRect(cw*2/3 - LANE_WIDTH, 0, LANE_WIDTH*2, ch);
  ctx.fillRect(0, ch/3 - LANE_WIDTH, cw, LANE_WIDTH*2);
  ctx.fillRect(0, ch*2/3 - LANE_WIDTH, cw, LANE_WIDTH*2);
  
  ctx.strokeStyle = '#2d3748';
  ctx.lineWidth = 1;
  ctx.setLineDash([15, 15]);
  ctx.beginPath();
  ctx.moveTo(cw/3, 0); ctx.lineTo(cw/3, ch);
  ctx.moveTo(cw*2/3, 0); ctx.lineTo(cw*2/3, ch);
  ctx.moveTo(0, ch/3); ctx.lineTo(cw, ch/3);
  ctx.moveTo(0, ch*2/3); ctx.lineTo(cw, ch*2/3);
  ctx.stroke();
  ctx.setLineDash([]); // Reset dash
  
  // Draw GPS Dijkstra Line
  if (activeVipRoute && activeVipRoute.length > 0) {
      ctx.strokeStyle = 'rgba(255, 0, 255, 0.4)';
      ctx.lineWidth = 10;
      ctx.lineCap = 'round';
      ctx.lineJoin = 'round';
      ctx.beginPath();
      
      let n0 = junctions.find(j => j.id === activeVipRoute[0]);
      if (n0) {
          if (n0.id === 'NW') ctx.moveTo(n0.cx - 15, 0); 
          else if (n0.id === 'SW') ctx.moveTo(0, n0.cy + 15); 
          else if (n0.id === 'SE') ctx.moveTo(n0.cx + 15, ch);
          else if (n0.id === 'NE') ctx.moveTo(cw, n0.cy - 15);
      }
      
      for (let i=0; i<activeVipRoute.length; i++) {
          let j = junctions.find(jx => jx.id === activeVipRoute[i]);
          if (!j) continue;
          let nextJ = i < activeVipRoute.length - 1 ? junctions.find(jx => jx.id === activeVipRoute[i+1]) : null;
          let prevJ = i > 0 ? junctions.find(jx => jx.id === activeVipRoute[i-1]) : null;
          
          let enterDir = 0; // Default North->South
          if (prevJ) {
              if (prevJ.cx < j.cx) enterDir = 3; // West->East
              else if (prevJ.cy > j.cy) enterDir = 2; // South->North
              else if (prevJ.cx > j.cx) enterDir = 1; // East->West
          } else if (n0) {
              // Assume enter dir based on initial start boundary logic
              if (n0.id === 'SW') enterDir = 3;
              else if (n0.id === 'SE') enterDir = 2;
              else if (n0.id === 'NE') enterDir = 1;
          }
          
          let exitDir = 0; // Default exit South
          if (nextJ) {
              if (nextJ.cx > j.cx) exitDir = 3; // West->East
              else if (nextJ.cy < j.cy) exitDir = 2; // South->North
              else if (nextJ.cx < j.cx) exitDir = 1; // East->West
          } else {
              // Assume exit dir based on final end boundary logic
              let jL = activeVipRoute[activeVipRoute.length-1];
              if (jL === 'NW') exitDir = 2;
              else if (jL === 'SW') exitDir = 1;
              else if (jL === 'NE') exitDir = 3;
          }
          
          // Draw connecting vectors through the intersection box based on enter/exit combos
          // This ensures perfect 90 degree bends exactly like the Vehicle collision mechanics
          let inX = j.cx + (enterDir===2?15 : enterDir===0?-15 : 0);
          let inY = j.cy + (enterDir===1?-15 : enterDir===3?15 : 0);
          
          let outX = j.cx + (exitDir===2?15 : exitDir===0?-15 : 0);
          let outY = j.cy + (exitDir===1?-15 : exitDir===3?15 : 0);
          
          ctx.lineTo(inX, inY); // Enter bounding box
          if (enterDir !== exitDir && (enterDir%2 !== exitDir%2)) {
             // 90 Deg turn: Match inner vertex to prevent diagonal cuts
             if (enterDir === 0 || enterDir === 2) ctx.lineTo(inX, outY);
             else ctx.lineTo(outX, inY);
          }
          ctx.lineTo(outX, outY); // Queue Exit
      }
      
      // Final segment to randomized exterior bound
      let jLast = junctions.find(jx => jx.id === activeVipRoute[activeVipRoute.length-1]);
      if (jLast) {
          if (jLast.id === 'SE') ctx.lineTo(jLast.cx - 15, ch);
          else if (jLast.id === 'NE') ctx.lineTo(cw, jLast.cy + 15);
          else if (jLast.id === 'NW') ctx.lineTo(jLast.cx + 15, 0);
          else if (jLast.id === 'SW') ctx.lineTo(0, jLast.cy - 15);
      }
      
      ctx.stroke();
  }
  
  const drawLight = (x, y, state) => {
    ctx.fillStyle = state ? '#00ff88' : '#ff3b55';
    ctx.shadowBlur = 8;
    ctx.shadowColor = ctx.fillStyle;
    ctx.beginPath(); ctx.arc(x, y, 5, 0, Math.PI*2); ctx.fill();
    ctx.shadowBlur = 0;
  };
  
  for (let j of junctions) {
      ctx.fillStyle = '#161d2a';
      ctx.fillRect(j.cx - LANE_WIDTH, j.cy - LANE_WIDTH, LANE_WIDTH*2, LANE_WIDTH*2);
      
      drawLight(j.cx - LANE_WIDTH - 12, j.cy - LANE_WIDTH - 12, j.state === 'NS');
      drawLight(j.cx + LANE_WIDTH + 12, j.cy + LANE_WIDTH + 12, j.state === 'NS');
      drawLight(j.cx + LANE_WIDTH + 12, j.cy - LANE_WIDTH - 12, j.state === 'EW');
      drawLight(j.cx - LANE_WIDTH - 12, j.cy + LANE_WIDTH + 12, j.state === 'EW');
      
      ctx.fillStyle = "rgba(255,255,255,0.2)";
      ctx.font = "12px monospace";
      ctx.fillText(j.id, j.cx - 8, j.cy + 4);
  }

  vehicles.forEach(v => v.draw(ctx));
  requestAnimationFrame(renderLoop);
}
