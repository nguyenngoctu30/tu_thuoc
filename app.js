// ── FIREBASE ──────────────────────────────────────────────────────────────────
import { initializeApp } from "https://www.gstatic.com/firebasejs/12.6.0/firebase-app.js";
import {
  getDatabase, ref, push, set, get, onValue
} from "https://www.gstatic.com/firebasejs/12.6.0/firebase-database.js";

const firebaseConfig = {
  apiKey: "AIzaSyDzQFoQ8R023siAj2qaKVneQHu_BII1Zlc",
  authDomain: "tuiot-ad770.firebaseapp.com",
  databaseURL: "https://tuiot-ad770-default-rtdb.asia-southeast1.firebasedatabase.app",
  projectId: "tuiot-ad770",
  storageBucket: "tuiot-ad770.firebasestorage.app",
  messagingSenderId: "174269480706",
  appId: "1:174269480706:web:96cbe14c0680f9f7c34c1b",
  measurementId: "G-S1XNWLX6EY"
};

const fbApp = initializeApp(firebaseConfig);
const db = getDatabase(fbApp);

// Firebase database refs
const boxesRef = ref(db, 'boxes');        // Trạng thái hộp từ ESP (lần cuối)
const schedulesRef = ref(db, 'schedules');    // Lịch uống đã gửi
const doseLogRef = ref(db, 'doseLog');      // Nhật ký đã uống
const activityLogRef = ref(db, 'activityLog');  // Nhật ký hoạt động

// ── MQTT CONFIG ───────────────────────────────────────────────────────────────
const MQTT_HOST = 'broker.hivemq.com';
const MQTT_PORT = 8884;   // WSS
const MQTT_USE_SSL = true;
const CLIENT_ID = 'web_hopThuoc_' + Math.random().toString(36).slice(2, 8);

const TOPICS_STATUS = ['esp/h1', 'esp/h2', 'esp/h3', 'esp/h4'];
const TOPIC_SCHEDULE = 'esp/schedule';

const DOSE_WINDOW_MIN = 30;

// ── STATE ─────────────────────────────────────────────────────────────────────
const state = {
  boxes: [
    { id: 1, label: 'Hộp 1', status: 'unknown', updatedAt: null },
    { id: 2, label: 'Hộp 2', status: 'unknown', updatedAt: null },
    { id: 3, label: 'Hộp 3', status: 'unknown', updatedAt: null },
    { id: 4, label: 'Hộp 4', status: 'unknown', updatedAt: null },
  ],
  timesPerDay: 2,
  timeSlots: ['07:00', '21:00'],
  mqttConnected: false,
  sentSchedules: [],
  selectedDate: todayStr(),
  scheduleDate: todayStr(),
  todayDoseLog: [],      // doseLog entries for today only
  allActivityLog: [],      // all activityLog entries from Firebase
  logFilter: 'all',
  logSearch: '',
};

let client = null;

// ── DOM REFS ──────────────────────────────────────────────────────────────────
const mqttDot = document.getElementById('mqttDot');
const mqttLabel = document.getElementById('mqttLabel');
const boxGrid = document.getElementById('boxGrid');
const timesVal = document.getElementById('timesPerDayVal');
const decreaseBtn = document.getElementById('decreaseBtn');
const increaseBtn = document.getElementById('increaseBtn');
const timeSlotsEl = document.getElementById('timeSlots');
const sendBtn = document.getElementById('sendScheduleBtn');
const sendFeedback = document.getElementById('sendFeedback');
const logBox = document.getElementById('logBox');
const clearLogBtn = document.getElementById('clearLogBtn');
const sentSchedulesEl = document.getElementById('sentSchedules');
const scheduleDateEl = document.getElementById('scheduleDate');
const logTimeline = document.getElementById('logTimeline');
const logCount = document.getElementById('logCount');
const logSearchEl = document.getElementById('logSearch');
const boxesLastUpdate = document.getElementById('boxesLastUpdate');

// ── HELPERS ───────────────────────────────────────────────────────────────────
function todayStr() {
  return new Date().toISOString().slice(0, 10);
}

function fmtTime() {
  const d = new Date();
  return [d.getHours(), d.getMinutes(), d.getSeconds()]
    .map(n => String(n).padStart(2, '0')).join(':');
}

function getBoxAssignedSlot(boxId) {
  const slots = state.timeSlots.slice(0, state.timesPerDay);
  if (slots.length === 0) return null;
  const slotIndex = (boxId - 1) % slots.length;
  return { slotIndex, time: slots[slotIndex], laneNum: slotIndex + 1 };
}

function isNearScheduledTime(slotTime) {
  if (!slotTime) return false;
  const [h, m] = slotTime.split(':').map(Number);
  const now = new Date();
  const nowMin = now.getHours() * 60 + now.getMinutes();
  const slotMin = h * 60 + m;
  return Math.abs(nowMin - slotMin) <= DOSE_WINDOW_MIN;
}

// ── TAB SWITCHING ─────────────────────────────────────────────────────────────
document.querySelectorAll('.tab-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    const tabId = btn.dataset.tab;
    document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
    btn.classList.add('active');
    document.getElementById(`tab-${tabId}`).classList.add('active');
    if (tabId === 'log') renderLogTimeline();
  });
});

// ── LOG FILTER & SEARCH ───────────────────────────────────────────────────────
document.querySelectorAll('.filter-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.filter-btn').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
    state.logFilter = btn.dataset.filter;
    renderLogTimeline();
  });
});

logSearchEl.addEventListener('input', () => {
  state.logSearch = logSearchEl.value.toLowerCase().trim();
  renderLogTimeline();
});

// ── RENDER BOX GRID ───────────────────────────────────────────────────────────
function renderBoxes() {
  boxGrid.innerHTML = '';
  state.boxes.forEach((box) => {
    const statusClass = box.status === 'has' ? 'has'
      : box.status === 'empty' ? 'empty' : 'unknown';
    const card = document.createElement('div');
    card.className = `box-card ${statusClass}`;
    card.setAttribute('data-box-id', box.id);

    const icon = box.status === 'has' ? '💊'
      : box.status === 'empty' ? '🫙' : '❓';
    const statusText = box.status === 'has' ? 'Còn thuốc'
      : box.status === 'empty' ? 'Hết thuốc' : 'Chưa có dữ liệu';
    const updatedTxt = box.updatedAt ? `Cập nhật: ${box.updatedAt}`
      : 'Chưa nhận tín hiệu';

    card.innerHTML = `
      <div class="box-label">Hộp ${box.id}</div>
      <div class="box-icon">${icon}</div>
      <div class="box-status">${statusText}</div>
      <div class="box-updated">${updatedTxt}</div>
    `;

    boxGrid.appendChild(card);
  });
}

function updateBoxCard(idx) {
  const card = boxGrid.querySelector(`[data-box-id="${state.boxes[idx].id}"]`);
  if (!card) { renderBoxes(); return; }

  const box = state.boxes[idx];
  const statusClass = box.status === 'has' ? 'has'
    : box.status === 'empty' ? 'empty' : 'unknown';
  card.className = `box-card ${statusClass}`;

  const icon = box.status === 'has' ? '💊'
    : box.status === 'empty' ? '🫙' : '❓';
  const statusText = box.status === 'has' ? 'Còn thuốc'
    : box.status === 'empty' ? 'Hết thuốc' : 'Chưa có dữ liệu';

  card.querySelector('.box-icon').textContent = icon;
  card.querySelector('.box-status').textContent = statusText;
  card.querySelector('.box-updated').textContent = `Cập nhật: ${box.updatedAt}`;
}

// ── MARK TAKEN ────────────────────────────────────────────────────────────────
function markTaken(boxId, source = 'auto') {
  const now = new Date();
  const entry = {
    boxId,
    date: todayStr(),
    takenAt: fmtTime(),
    timestamp: now.toISOString(),
    status: source,          // 'auto' | 'manual'
  };

  push(doseLogRef, entry)
    .then(() => {
      addLog(`Hộp ${boxId}: đã uống ✅ (${source === 'manual' ? 'thủ công' : 'tự động'})`, 'ok');
    })
    .catch(err => addLog(`Lỗi ghi dose log: ${err.message}`, 'error'));
}

// ── TIME SLOTS ────────────────────────────────────────────────────────────────
function renderTimeSlots() {
  timeSlotsEl.innerHTML = '';
  for (let i = 0; i < state.timesPerDay; i++) {
    const val = state.timeSlots[i] || defaultTimeSlot(i, state.timesPerDay);
    const row = document.createElement('div');
    row.className = 'time-slot-row';
    row.innerHTML = `
      <span class="time-slot-label">Lần ${i + 1}</span>
      <input class="field-input" type="time" value="${val}" data-slot="${i}" style="width:120px" />
    `;
    timeSlotsEl.appendChild(row);
  }
  state.timeSlots = Array.from({ length: state.timesPerDay }, (_, i) =>
    state.timeSlots[i] || defaultTimeSlot(i, state.timesPerDay)
  );
  timeSlotsEl.querySelectorAll('input[type="time"]').forEach(input => {
    input.addEventListener('change', e => {
      const idx = parseInt(e.target.dataset.slot);
      if (!Number.isNaN(idx)) state.timeSlots[idx] = e.target.value;
    });
  });
}

function defaultTimeSlot(index, total) {
  if (total === 1) return '08:00';
  if (total === 2) return index === 0 ? '07:00' : '21:00';
  const presets = ['07:00', '12:00', '18:00', '21:00', '06:00', '09:00'];
  return presets[index] ?? `${String(6 + index * 3).padStart(2, '0')}:00`;
}

function applyDatePicker() {
  if (!scheduleDateEl) return;
  scheduleDateEl.value = state.scheduleDate;
  scheduleDateEl.addEventListener('change', () => {
    state.scheduleDate = scheduleDateEl.value;
    state.selectedDate = scheduleDateEl.value;
    renderCalendar();
    renderDayDetails();
  });
}

// ── STEPPER ───────────────────────────────────────────────────────────────────
function updateTimesPerDay(delta) {
  const next = Math.min(6, Math.max(1, state.timesPerDay + delta));
  if (next === state.timesPerDay) return;
  state.timesPerDay = next;
  timesVal.textContent = next;
  decreaseBtn.disabled = next <= 1;
  increaseBtn.disabled = next >= 6;
  renderTimeSlots();
}
decreaseBtn.addEventListener('click', () => updateTimesPerDay(-1));
increaseBtn.addEventListener('click', () => updateTimesPerDay(+1));
timesVal.textContent = state.timesPerDay;

// ── SEND SCHEDULE ─────────────────────────────────────────────────────────────
function sendSchedule() {
  if (!state.mqttConnected) {
    addLog('Chưa kết nối MQTT – không thể gửi lịch!', 'error');
    return;
  }

  timeSlotsEl.querySelectorAll('input[type="time"]').forEach(input => {
    const idx = parseInt(input.dataset.slot);
    if (!Number.isNaN(idx)) state.timeSlots[idx] = input.value;
  });

  const dateValue = scheduleDateEl?.value || state.scheduleDate;
  state.scheduleDate = dateValue;
  state.selectedDate = dateValue;
  if (scheduleDateEl) scheduleDateEl.value = dateValue;

  const payload = JSON.stringify({
    date: dateValue,
    times: state.timesPerDay,
    slots: state.timeSlots.slice(0, state.timesPerDay),
  });

  const msg = new Paho.Message(payload);
  msg.destinationName = TOPIC_SCHEDULE;
  msg.retained = true;

  try {
    client.send(msg);
    addLog(`Đã gửi lịch → ${TOPIC_SCHEDULE}: ${payload}`, 'ok');
    showFeedback('✓ Đã gửi xuống ESP32');

    const scheduleData = safeSchedule({
      date: dateValue,
      time: fmtTime(),
      times: state.timesPerDay,
      slots: state.timeSlots.slice(0, state.timesPerDay),
      timestamp: new Date().toISOString(),
    });

    // Lưu vào Firebase /schedules
    push(schedulesRef, scheduleData)
      .then(() => addLog('Đã lưu lịch vào Firebase ✓', 'ok'))
      .catch(err => addLog(`Lỗi lưu Firebase: ${err.message}`, 'error'));

  } catch (e) {
    addLog(`Lỗi gửi MQTT: ${e.message}`, 'error');
  }
}
sendBtn.addEventListener('click', sendSchedule);

function showFeedback(text) {
  sendFeedback.textContent = text;
  sendFeedback.classList.add('visible');
  setTimeout(() => sendFeedback.classList.remove('visible'), 3000);
}

// ── SAFE SCHEDULE ─────────────────────────────────────────────────────────────
function safeSchedule(schedule) {
  const slots = Array.isArray(schedule?.slots) ? schedule.slots : [];
  const timestamp = schedule?.timestamp ? new Date(schedule.timestamp) : new Date(NaN);
  let dateValue = null;

  if (typeof schedule?.date === 'string') {
    const p = new Date(schedule.date);
    if (!Number.isNaN(p.getTime())) dateValue = p.toISOString().split('T')[0];
  }
  if (!dateValue && !Number.isNaN(timestamp.getTime())) {
    dateValue = timestamp.toISOString().split('T')[0];
  }

  return {
    date: dateValue,
    time: schedule?.time || '',
    times: typeof schedule?.times === 'number' ? schedule.times : slots.length,
    slots,
    timestamp: Number.isNaN(timestamp.getTime()) ? null : timestamp.toISOString(),
  };
}

// ── RENDER SENT SCHEDULES ─────────────────────────────────────────────────────
function renderSentSchedules() {
  sentSchedulesEl.innerHTML = '';
  if (state.sentSchedules.length === 0) {
    sentSchedulesEl.innerHTML = '<p class="no-schedules">Chưa có lịch nào được gửi...</p>';
    return;
  }
  state.sentSchedules.slice(0, 8).forEach(schedule => {
    const slots = Array.isArray(schedule.slots) ? schedule.slots : [];
    const dateLabel = schedule.date
      || (schedule.timestamp ? schedule.timestamp.split('T')[0] : '---');
    const div = document.createElement('div');
    div.className = 'sent-schedule-item';
    div.innerHTML = `
      <div class="sent-schedule-icon">📅</div>
      <div>
        <div class="sent-time">${dateLabel} &nbsp;&middot;&nbsp; Gửi lúc ${schedule.time || '---'}</div>
        <div class="sent-details">${schedule.times || slots.length} lần/ngày &nbsp;&middot;&nbsp; ${slots.join('  ·  ')}</div>
      </div>
    `;
    sentSchedulesEl.appendChild(div);
  });
}

// ── CALENDAR ─────────────────────────────────────────────────────────────────
function renderCalendar() {
  const calendarEl = document.getElementById('calendar');
  const now = new Date();
  const year = now.getFullYear();
  const month = now.getMonth();
  const monthNames = ['Tháng 1', 'Tháng 2', 'Tháng 3', 'Tháng 4', 'Tháng 5', 'Tháng 6',
    'Tháng 7', 'Tháng 8', 'Tháng 9', 'Tháng 10', 'Tháng 11', 'Tháng 12'];

  calendarEl.innerHTML = `<div class="cal-header"><h3>${monthNames[month]} ${year}</h3></div>`;

  const grid = document.createElement('div');
  grid.className = 'cal-grid';

  ['CN', 'T2', 'T3', 'T4', 'T5', 'T6', 'T7'].forEach(d => {
    const el = document.createElement('div');
    el.className = 'cal-dow';
    el.textContent = d;
    grid.appendChild(el);
  });

  const firstDay = new Date(year, month, 1).getDay();
  for (let i = 0; i < firstDay; i++) {
    const e = document.createElement('div');
    e.className = 'cal-day cal-day--empty';
    grid.appendChild(e);
  }

  const daysInMonth = new Date(year, month + 1, 0).getDate();
  const daysWithSch = getDaysWithSchedules();
  const todayDate = todayStr();

  for (let day = 1; day <= daysInMonth; day++) {
    const dateStr = `${year}-${String(month + 1).padStart(2, '0')}-${String(day).padStart(2, '0')}`;
    const hasSch = daysWithSch.includes(dateStr);
    const isToday = dateStr === todayDate;
    const isSelected = dateStr === state.selectedDate;

    const d = document.createElement('div');
    d.className = `cal-day${isSelected ? ' cal-day--selected'
      : isToday ? ' cal-day--today'
        : hasSch ? ' cal-day--has' : ''}`;
    d.innerHTML = `<span>${day}</span>${hasSch ? '<span class="cal-dot"></span>' : ''}`;
    d.title = hasSch ? 'Có lịch uống thuốc' : '';
    d.addEventListener('click', () => {
      state.selectedDate = dateStr;
      state.scheduleDate = dateStr;
      if (scheduleDateEl) scheduleDateEl.value = dateStr;
      renderCalendar();
      renderDayDetails();
    });
    grid.appendChild(d);
  }

  calendarEl.appendChild(grid);
}

function getDaysWithSchedules() {
  const days = new Set();
  state.sentSchedules.forEach(s => {
    const d = s.date || (s.timestamp ? new Date(s.timestamp).toISOString().split('T')[0] : null);
    if (d) days.add(d);
  });
  return Array.from(days);
}

function getLatestScheduleForDate(date) {
  return state.sentSchedules
    .filter(s => {
      const d = s.date || (s.timestamp ? new Date(s.timestamp).toISOString().split('T')[0] : null);
      return d === date;
    })
    .sort((a, b) => {
      const ta = a.timestamp ? new Date(a.timestamp).getTime() : 0;
      const tb = b.timestamp ? new Date(b.timestamp).getTime() : 0;
      return tb - ta;
    })[0] || null;
}

function renderDayDetails() {
  const detailsEl = document.getElementById('dayDetails');
  if (!state.selectedDate) {
    detailsEl.innerHTML = '<p class="text-hint">Chọn một ngày để xem chi tiết lịch uống thuốc.</p>';
    return;
  }
  const schedule = getLatestScheduleForDate(state.selectedDate);
  if (!schedule) {
    detailsEl.innerHTML = `<p class="text-hint">Ngày ${state.selectedDate} không có lịch uống thuốc.</p>`;
    return;
  }
  // Fix timezone offset when parsing date-only string
  const dateObj = new Date(state.selectedDate + 'T00:00:00');
  const dayNames = ['Chủ Nhật', 'Thứ Hai', 'Thứ Ba', 'Thứ Tư', 'Thứ Năm', 'Thứ Sáu', 'Thứ Bảy'];
  const fmtDate = `${dayNames[dateObj.getDay()]}, ${dateObj.getDate()}/${dateObj.getMonth() + 1}/${dateObj.getFullYear()}`;
  const slots = Array.isArray(schedule.slots) ? schedule.slots : [];

  detailsEl.innerHTML = `
    <div class="day-detail-card">
      <div class="day-detail-date">${fmtDate}</div>
      <div class="day-detail-info">
        <span>Gửi lúc: ${schedule.time || '---'}</span>
        <span class="day-detail-times">${schedule.times || slots.length} lần/ngày</span>
      </div>
      <div class="day-detail-slots">
        ${slots.map(s => `<span class="slot-chip">🕐 ${s}</span>`).join('')}
      </div>
    </div>
  `;
}

// ── MINI LOG ─────────────────────────────────────────────────────────────────
function addLog(msg, type = '') {
  // Xóa placeholder nếu còn
  const ph = logBox.querySelector('.log-placeholder');
  if (ph) ph.remove();

  const t = fmtTime();
  const entry = document.createElement('div');
  entry.className = 'log-entry';
  const colorClass = type === 'ok' ? 'ok' : type === 'error' ? 'error' : type === 'warn' ? 'warn' : '';
  entry.innerHTML = `<span class="log-time">${t}</span><span class="log-msg ${colorClass}">${msg}</span>`;
  logBox.appendChild(entry);
  logBox.scrollTop = logBox.scrollHeight;

  // Lưu vào Firebase activityLog (không await – fire & forget)
  push(activityLogRef, {
    message: msg,
    type: type,
    timestamp: new Date().toISOString(),
  }).catch(() => { }); // silent fail để tránh vòng lặp
}

clearLogBtn.addEventListener('click', () => {
  logBox.innerHTML = '<p class="log-placeholder">Chưa có sự kiện nào...</p>';
});

// ── RENDER LOG TIMELINE ───────────────────────────────────────────────────────
function renderLogTimeline() {
  if (!logTimeline) return;

  let entries = [...state.allActivityLog];

  if (state.logFilter !== 'all') {
    entries = entries.filter(e => e.type === state.logFilter);
  }
  if (state.logSearch) {
    entries = entries.filter(e =>
      (e.message || '').toLowerCase().includes(state.logSearch)
    );
  }

  if (logCount) logCount.textContent = `${entries.length} mục`;

  if (entries.length === 0) {
    logTimeline.innerHTML = '<p class="log-placeholder">Không tìm thấy mục nào phù hợp.</p>';
    return;
  }

  // Nhóm theo ngày
  const groups = {};
  entries.forEach(e => {
    const date = e.timestamp ? e.timestamp.slice(0, 10) : 'unknown';
    if (!groups[date]) groups[date] = [];
    groups[date].push(e);
  });

  logTimeline.innerHTML = '';

  const dayNames = ['Chủ Nhật', 'Thứ Hai', 'Thứ Ba', 'Thứ Tư', 'Thứ Năm', 'Thứ Sáu', 'Thứ Bảy'];
  const today = todayStr();
  const yesterday = new Date(Date.now() - 86400000).toISOString().slice(0, 10);

  Object.keys(groups)
    .sort((a, b) => b.localeCompare(a))
    .forEach(date => {
      const group = document.createElement('div');
      group.className = 'timeline-group';

      const lbl = document.createElement('div');
      lbl.className = 'timeline-date-label';
      if (date === today) {
        lbl.textContent = 'Hôm nay';
      } else if (date === yesterday) {
        lbl.textContent = 'Hôm qua';
      } else if (date !== 'unknown') {
        const d = new Date(date + 'T00:00:00');
        lbl.textContent = `${dayNames[d.getDay()]} – ${d.getDate()}/${d.getMonth() + 1}/${d.getFullYear()}`;
      } else {
        lbl.textContent = 'Không xác định';
      }
      group.appendChild(lbl);

      const typeIcon = { ok: '✅', warn: '⚠️', error: '❌', '': 'ℹ️' };

      groups[date]
        .sort((a, b) => (b.timestamp || '').localeCompare(a.timestamp || ''))
        .forEach(e => {
          const row = document.createElement('div');
          row.className = 'timeline-entry';
          const timeStr = e.timestamp
            ? new Date(e.timestamp).toLocaleTimeString('vi-VN', { hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false })
            : '--:--:--';
          const icon = typeIcon[e.type] ?? 'ℹ️';
          const msgClass = e.type || '';
          row.innerHTML = `
            <span class="timeline-time">${timeStr}</span>
            <span class="timeline-icon ${e.type || 'info'}">${icon}</span>
            <span class="timeline-msg ${msgClass}">${e.message || ''}</span>
          `;
          group.appendChild(row);
        });

      logTimeline.appendChild(group);
    });
}

// ── FIREBASE REAL-TIME LISTENERS ──────────────────────────────────────────────

/** Lắng nghe trạng thái hộp từ Firebase (cập nhật khi ESP ghi qua MQTT) */
function setupBoxListener() {
  onValue(boxesRef, snapshot => {
    if (!snapshot.exists()) return;
    const data = snapshot.val();
    ['box1', 'box2', 'box3', 'box4'].forEach((key, i) => {
      if (data[key]) {
        state.boxes[i].status = data[key].status || 'unknown';
        state.boxes[i].updatedAt = data[key].updatedAt
          ? new Date(data[key].updatedAt).toLocaleTimeString('vi-VN')
          : null;
      }
    });
    renderBoxes();
    if (boxesLastUpdate) {
      boxesLastUpdate.textContent = `Đồng bộ lúc ${fmtTime()}`;
    }
  });
}

/** Lắng nghe lịch đã gửi từ Firebase */
function setupScheduleListener() {
  onValue(schedulesRef, snapshot => {
    if (!snapshot.exists()) {
      state.sentSchedules = [];
      renderSentSchedules();
      renderCalendar();
      renderDayDetails();
      return;
    }
    const data = snapshot.val();
    state.sentSchedules = Object.values(data)
      .map(safeSchedule)
      .filter(s => s.timestamp !== null)
      .sort((a, b) => new Date(b.timestamp) - new Date(a.timestamp));
    renderSentSchedules();
    renderCalendar();
    renderDayDetails();
  });
}



/** Lắng nghe toàn bộ activity log từ Firebase */
function setupActivityLogListener() {
  onValue(activityLogRef, snapshot => {
    if (!snapshot.exists()) {
      state.allActivityLog = [];
    } else {
      const data = snapshot.val();
      state.allActivityLog = Object.values(data)
        .sort((a, b) => (b.timestamp || '').localeCompare(a.timestamp || ''));
      // Chỉ giữ 500 mục gần nhất trong bộ nhớ
      if (state.allActivityLog.length > 500) {
        state.allActivityLog = state.allActivityLog.slice(0, 500);
      }
    }
    if (logCount) logCount.textContent = `${state.allActivityLog.length} mục`;
    // Nếu tab Log đang mở thì render lại
    const logTab = document.getElementById('tab-log');
    if (logTab && logTab.classList.contains('active')) {
      renderLogTimeline();
    }
  });
}

// ── MQTT ─────────────────────────────────────────────────────────────────────
function setMqttStatus(connected, label) {
  state.mqttConnected = connected;
  if (connected) {
    mqttDot.className = 'dot dot--ok';
    mqttLabel.style.color = 'var(--green-600)';
  } else {
    mqttDot.className = 'dot dot--error';
    mqttLabel.style.color = 'var(--red-600)';
  }
  mqttLabel.textContent = label;
}

function initMqtt() {
  client = new Paho.Client(MQTT_HOST, MQTT_PORT, CLIENT_ID);

  client.onConnectionLost = resp => {
    setMqttStatus(false, 'Mất kết nối');
    addLog(`MQTT mất kết nối: ${resp.errorMessage}`, 'error');
    setTimeout(connectMqtt, 5000);
  };

  client.onMessageArrived = msg => {
    handleMqttMessage(msg.destinationName, msg.payloadString.trim());
  };

  connectMqtt();
}

function connectMqtt() {
  mqttDot.className = 'dot dot--off';
  mqttLabel.textContent = 'Đang kết nối...';
  mqttLabel.style.color = '';
  addLog(`Kết nối tới ${MQTT_HOST}:${MQTT_PORT} (WSS)...`);

  client.connect({
    useSSL: MQTT_USE_SSL,
    onSuccess: onConnect,
    onFailure: onConnectFail,
    keepAliveInterval: 30,
    timeout: 10,
  });
}

function onConnect() {
  setMqttStatus(true, 'Đã kết nối');
  addLog('MQTT kết nối thành công ✓', 'ok');
  TOPICS_STATUS.forEach(topic => {
    client.subscribe(topic);
    addLog(`Đăng ký topic: ${topic}`);
  });
  client.subscribe('esp/schedule/ack');
}

function onConnectFail(resp) {
  setMqttStatus(false, 'Kết nối thất bại');
  addLog(`Kết nối MQTT thất bại (${resp.errorMessage}) – thử lại sau 5s`, 'error');
  setTimeout(connectMqtt, 5000);
}

function showWarningToast(boxId, scheduledTime) {
  const prev = document.getElementById('webWarningToast');
  if (prev) prev.remove();

  const toast = document.createElement('div');
  toast.id = 'webWarningToast';
  toast.className = 'early-warning-toast';
  toast.innerHTML = `
    <div class="ewt-icon">🚨</div>
    <div class="ewt-body">
      <div class="ewt-title">Cảnh báo lấy thuốc sớm!</div>
      <div class="ewt-msg">
        Bạn vừa lấy thuốc tại <strong>Hộp ${boxId}</strong> khi <strong>chưa đến giờ uống</strong> (Giờ quy định: ${scheduledTime}).
      </div>
    </div>
    <button class="ewt-close" onclick="this.parentElement.remove()">✕</button>
  `;
  document.body.appendChild(toast);

  // Tự động xóa sau 8 giây
  setTimeout(() => {
    if (toast.isConnected) {
      toast.style.opacity = '0';
      toast.style.transform = 'translateY(-20px)';
      setTimeout(() => toast.remove(), 400);
    }
  }, 8000);
}

function handleMqttMessage(topic, payload) {
  // ── Trạng thái hộp: esp/h1 .. esp/h4 ──
  const boxIdx = TOPICS_STATUS.indexOf(topic);
  if (boxIdx !== -1) {
    const hasmed = payload === '1';
    const box = state.boxes[boxIdx];
    const prevStatus = box.status;
    box.status = hasmed ? 'has' : 'empty';
    box.updatedAt = fmtTime();

    // Lưu trạng thái lên Firebase /boxes/boxN
    const boxKey = `box${box.id}`;
    set(ref(db, `boxes/${boxKey}`), {
      status: box.status,
      updatedAt: new Date().toISOString(),
    }).catch(err => console.warn('Firebase box update error:', err));

    // Cập nhật card ngay lập tức
    updateBoxCard(boxIdx);

    // Phát hiện has → empty = đã lấy thuốc ra (tự động)
    if (prevStatus === 'has' && box.status === 'empty') {
      const assigned = getBoxAssignedSlot(box.id);
      if (assigned && !isNearScheduledTime(assigned.time)) {
        const nowStr = new Date().toLocaleTimeString('vi-VN', { hour: '2-digit', minute: '2-digit', hour12: false });
        addLog(`🚨 Cảnh báo: ${box.label} bị lấy thuốc sớm! (Hiện tại: ${nowStr} | Giờ quy định: ${assigned.time})`, 'error');
        showWarningToast(box.id, assigned.time);
      } else {
        addLog(`${box.label}: đã lấy thuốc đúng giờ ✅`, 'ok');
      }
      markTaken(box.id, 'auto');
    } else if (prevStatus !== box.status) {
      const logMsg = hasmed
        ? `${box.label}: còn thuốc ✓`
        : `${box.label}: hết thuốc ⚠`;
      addLog(logMsg, hasmed ? 'ok' : 'warn');
    }
    return;
  }

  // ── ACK từ ESP32 ──
  if (topic === 'esp/schedule/ack') {
    addLog(`ESP32 xác nhận lịch: ${payload}`, 'ok');
    return;
  }

  addLog(`[${topic}] ${payload}`);
}

// ── INIT ──────────────────────────────────────────────────────────────────────
renderBoxes();
renderTimeSlots();
applyDatePicker();
renderSentSchedules();
renderCalendar();
renderDayDetails();

// Thiết lập real-time listeners Firebase
setupBoxListener();
setupScheduleListener();
setupActivityLogListener();

// Kết nối MQTT
initMqtt();

// ── MOUSE EFFECTS ─────────────────────────────────────────────────────────────
// Chỉ kích hoạt trên thiết bị có chuột thật
if (window.matchMedia('(hover: hover) and (pointer: fine)').matches) {
  (function initMouseEffects() {
    const dot = document.getElementById('cursorDot');
    const glow = document.getElementById('cursorGlow');
    if (!dot || !glow) return;

    let mx = -300, my = -300;   // vị trí chuột thực
    let gx = -300, gy = -300;   // vị trí glow (lag)
    let rafId;

    // ── Dot theo ngay lập tức ──────────────────────────────
    document.addEventListener('mousemove', e => {
      mx = e.clientX;
      my = e.clientY;

      // Dot snap ngay
      dot.style.left = mx + 'px';
      dot.style.top = my + 'px';

      // CSS var cho spotlight nền
      document.documentElement.style.setProperty('--cursor-x', mx + 'px');
      document.documentElement.style.setProperty('--cursor-y', my + 'px');
    });

    // ── Glow lag (easing) ──────────────────────────────────
    function animateGlow() {
      gx += (mx - gx) * 0.065;
      gy += (my - gy) * 0.065;
      glow.style.left = gx + 'px';
      glow.style.top = gy + 'px';
      rafId = requestAnimationFrame(animateGlow);
    }
    animateGlow();

    // ── Expand dot trên phần tử tương tác ─────────────────
    const INTERACTIVE = 'button, a, input, select, label, ' +
      '.box-card, .cal-day, .filter-btn, .tab-btn, ' +
      '.sent-schedule-item, .mark-taken-btn, .ghost-btn, .btn--glow';

    document.addEventListener('mouseover', e => {
      if (e.target.closest(INTERACTIVE)) {
        dot.classList.add('expanded');
      } else {
        dot.classList.remove('expanded');
      }
    });

    // ── Thu nhỏ khi nhấn ──────────────────────────────────
    document.addEventListener('mousedown', () => dot.classList.add('clicking'));
    document.addEventListener('mouseup', () => dot.classList.remove('clicking'));

    // ── Ẩn/hiện khi chuột vào/ra cửa sổ ──────────────────
    document.addEventListener('mouseleave', () => {
      dot.style.opacity = '0';
      glow.style.opacity = '0';
    });
    document.addEventListener('mouseenter', () => {
      dot.style.opacity = '1';
      glow.style.opacity = '1';
    });

    // ── Click ripple ───────────────────────────────────────
    document.addEventListener('click', e => {
      const r = document.createElement('div');
      r.className = 'click-ripple';
      r.style.left = e.clientX + 'px';
      r.style.top = e.clientY + 'px';
      document.body.appendChild(r);
      setTimeout(() => r.remove(), 600);
    });

    // ── 3D Tilt + Shimmer trên các card ───────────────────
    function bindCardEffects() {
      // Box cards – tilt 3D + shimmer
      document.querySelectorAll('.box-card').forEach(card => {
        if (card._tiltBound) return;
        card._tiltBound = true;

        card.addEventListener('mousemove', e => {
          const r = card.getBoundingClientRect();
          const xr = (e.clientX - r.left) / r.width - 0.5;  // -0.5 → 0.5
          const yr = (e.clientY - r.top) / r.height - 0.5;
          card.style.transform =
            `perspective(700px) rotateY(${xr * 14}deg) rotateX(${-yr * 14}deg) ` +
            `translateY(-5px) scale(1.025)`;
          // Shimmer theo con trỏ
          card.style.setProperty('--cx', ((e.clientX - r.left) / r.width * 100) + '%');
          card.style.setProperty('--cy', ((e.clientY - r.top) / r.height * 100) + '%');
        });

        card.addEventListener('mouseleave', () => {
          card.style.transform = '';
          // Reset shimmer position ra ngoài card
          card.style.setProperty('--cx', '-50%');
          card.style.setProperty('--cy', '-50%');
        });
      });

      // Glass cards – chỉ shimmer (không tilt vì có form inputs bên trong)
      document.querySelectorAll('.glass-card').forEach(card => {
        if (card._shimmerBound) return;
        card._shimmerBound = true;

        card.addEventListener('mousemove', e => {
          const r = card.getBoundingClientRect();
          card.style.setProperty('--cx', ((e.clientX - r.left) / r.width * 100) + '%');
          card.style.setProperty('--cy', ((e.clientY - r.top) / r.height * 100) + '%');
        });

        card.addEventListener('mouseleave', () => {
          card.style.setProperty('--cx', '-50%');
          card.style.setProperty('--cy', '-50%');
        });
      });
    }

    // Bind ngay và mỗi khi boxGrid thay đổi (Firebase re-render)
    bindCardEffects();
    const observer = new MutationObserver(() => setTimeout(bindCardEffects, 50));
    const grid = document.getElementById('boxGrid');
    if (grid) observer.observe(grid, { childList: true, subtree: false });

  })();
}
