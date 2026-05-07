// ── FIREBASE ──────────────────────────────────────────────
import { initializeApp } from "https://www.gstatic.com/firebasejs/12.6.0/firebase-app.js";
import { getDatabase, ref, push, set, get, remove } from "https://www.gstatic.com/firebasejs/12.6.0/firebase-database.js";

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

const app = initializeApp(firebaseConfig);
const db = getDatabase(app);
const ordersRef = ref(db, 'orders');

// ── CẤU HÌNH MQTT ────────────────────────────────────────
const MQTT_HOST    = 'broker.hivemq.com';
const MQTT_PORT    = 8884;          // WebSocket TLS (wss)
const MQTT_USE_SSL = true;
const CLIENT_ID    = 'web_hopThuoc_' + Math.random().toString(36).slice(2, 8);

// Topic lắng nghe trạng thái hộp từ ESP32
const TOPICS_STATUS = ['esp/h1', 'esp/h2', 'esp/h3', 'esp/h4'];

// Topic gửi cấu hình xuống ESP32
// Payload JSON: {"times":2,"slots":["07:00","21:00"]}
const TOPIC_SCHEDULE = 'esp/schedule';

// ── STATE ────────────────────────────────────────────────
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
  selectedDate: new Date().toISOString().slice(0, 10),
  scheduleDate: new Date().toISOString().slice(0, 10),
};

let client = null;

// ── DOM REFS ──────────────────────────────────────────────
const mqttDot       = document.getElementById('mqttDot');
const mqttLabel     = document.getElementById('mqttLabel');
const boxGrid       = document.getElementById('boxGrid');
const timesVal      = document.getElementById('timesPerDayVal');
const decreaseBtn   = document.getElementById('decreaseBtn');
const increaseBtn   = document.getElementById('increaseBtn');
const timeSlotsEl   = document.getElementById('timeSlots');
const sendBtn       = document.getElementById('sendScheduleBtn');
const sendFeedback  = document.getElementById('sendFeedback');
const logBox        = document.getElementById('logBox');
const clearLogBtn   = document.getElementById('clearLogBtn');
const sentSchedulesEl = document.getElementById('sentSchedules');
const scheduleDateEl = document.getElementById('scheduleDate');

// ── RENDER BOX GRID ───────────────────────────────────────
function renderBoxes() {
  boxGrid.innerHTML = '';
  state.boxes.forEach((box, i) => {
    const card = document.createElement('div');
    const cls = box.status === 'has' ? 'bg-green-100 border-green-300 text-green-800' : box.status === 'empty' ? 'bg-red-100 border-red-300 text-red-800' : 'bg-gray-100 border-gray-300 text-gray-800';
    card.className = `p-4 rounded-xl border-2 ${cls} transform hover:scale-105 transition-transform duration-200 shadow-md hover:shadow-lg`;
    card.setAttribute('data-index', i);

    const icon = box.status === 'has' ? '💊' : box.status === 'empty' ? '🫙' : '❓';
    const status = box.status === 'has' ? 'Còn thuốc' : box.status === 'empty' ? 'Hết thuốc' : 'Chưa có dữ liệu';
    const updated = box.updatedAt ? `Cập nhật: ${box.updatedAt}` : 'Chưa nhận tín hiệu';

    card.innerHTML = `
      <div class="text-center">
        <div class="text-3xl mb-2">${icon}</div>
        <div class="font-semibold text-lg">${box.label}</div>
        <div class="text-sm">${status}</div>
        <div class="text-xs mt-1 opacity-75">${updated}</div>
      </div>
    `;
    boxGrid.appendChild(card);
  });
}

// ── RENDER TIME SLOTS ─────────────────────────────────────
function renderTimeSlots() {
  timeSlotsEl.innerHTML = '';
  for (let i = 0; i < state.timesPerDay; i++) {
    const val = state.timeSlots[i] || defaultTimeSlot(i, state.timesPerDay);
    const row = document.createElement('div');
    row.className = 'flex items-center justify-between bg-white p-3 rounded-lg shadow-sm';
    row.innerHTML = `
      <span class="text-sm font-medium text-gray-700">Lần ${i + 1}</span>
      <input class="border border-gray-300 rounded px-2 py-1 focus:outline-none focus:ring-2 focus:ring-blue-500" type="time" value="${val}" data-slot="${i}" />
    `;
    timeSlotsEl.appendChild(row);
  }
  // Cập nhật state.timeSlots để khớp với số lần
  state.timeSlots = Array.from({ length: state.timesPerDay }, (_, i) =>
    state.timeSlots[i] || defaultTimeSlot(i, state.timesPerDay)
  );
  // Lắng nghe thay đổi
  timeSlotsEl.querySelectorAll('input[type="time"]').forEach(input => {
    input.addEventListener('change', e => {
      const idx = parseInt(e.target.dataset.slot);
      if (!Number.isNaN(idx)) {
        state.timeSlots[idx] = e.target.value;
      }
    });
  });
}

function syncCalendarDateToPicker() {
  if (scheduleDateEl) {
    scheduleDateEl.value = state.scheduleDate;
  }
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

function defaultTimeSlot(index, total) {
  // Phân bố đều trong ngày: 08:00, 14:00, 20:00…
  const presets = ['07:00', '12:00', '18:00', '21:00', '06:00', '09:00'];
  if (total === 1) return '08:00';
  if (total === 2) return index === 0 ? '07:00' : '21:00';
  return presets[index] || `0${6 + index * 3}:00`.slice(-5);
}

function safeSchedule(schedule) {
  const slots = Array.isArray(schedule?.slots) ? schedule.slots : [];
  const timestamp = schedule?.timestamp ? new Date(schedule.timestamp) : new Date(NaN);
  let dateValue = null;
  if (typeof schedule?.date === 'string') {
    const parsedDate = new Date(schedule.date);
    if (!Number.isNaN(parsedDate.getTime())) {
      dateValue = parsedDate.toISOString().split('T')[0];
    }
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

function ordinalLabel(n) {
  return ''; // Tiếng Việt không cần ordinal suffix
}

// ── STEPPER ───────────────────────────────────────────────
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

// ── GỬI LỊCH XUỐNG ESP32 ─────────────────────────────────
function sendSchedule() {
  if (!state.mqttConnected) {
    addLog('Chưa kết nối MQTT – không thể gửi lịch!', 'error');
    return;
  }
  // Đọc lại giá trị từ DOM (phòng người dùng chưa tab ra)
  timeSlotsEl.querySelectorAll('input[type="time"]').forEach(input => {
    const idx = parseInt(input.dataset.slot);
    if (!Number.isNaN(idx)) {
      state.timeSlots[idx] = input.value;
    }
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

    // Add to sent schedules
    const now = new Date();
    const timeStr = `${String(now.getHours()).padStart(2,'0')}:${String(now.getMinutes()).padStart(2,'0')}:${String(now.getSeconds()).padStart(2,'0')}`;
    const scheduleData = safeSchedule({
      date: dateValue,
      time: timeStr,
      times: state.timesPerDay,
      slots: state.timeSlots.slice(0, state.timesPerDay),
      timestamp: now.toISOString(),
    });
      state.sentSchedules.unshift(scheduleData);
      if (state.sentSchedules.length > 30) state.sentSchedules.pop(); // limit history for memory
      renderSentSchedules();
      renderCalendar();
      renderDayDetails();

    // Save to Firebase
    const newOrderRef = push(ordersRef);
    set(newOrderRef, scheduleData)
      .then(() => {
        addLog('Đã lưu lịch vào Firebase ✓', 'ok');
      })
      .catch((error) => {
        addLog(`Lỗi lưu Firebase: ${error.message}`, 'error');
      });

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

// ── RENDER SENT SCHEDULES ─────────────────────────────────
function renderSentSchedules() {
  sentSchedulesEl.innerHTML = '';
  if (state.sentSchedules.length === 0) {
    sentSchedulesEl.innerHTML = '<p class="text-gray-500 italic">Chưa có lịch nào được gửi...</p>';
    return;
  }
  const recent = state.sentSchedules.slice(0, 5);
  recent.forEach((schedule) => {
    const slots = Array.isArray(schedule.slots) ? schedule.slots : [];
    const dateLabel = schedule.date || (schedule.timestamp ? schedule.timestamp.split('T')[0] : '---');
    const div = document.createElement('div');
    div.className = 'bg-white p-4 rounded-lg shadow-sm hover:shadow-md transition-shadow';
    div.innerHTML = `
      <div class="text-sm text-blue-600 font-semibold">Ngày: ${dateLabel}</div>
      <div class="text-sm text-gray-600">Gửi lúc: ${schedule.time || '---'}</div>
      <div class="text-gray-800">${schedule.times || slots.length} lần/ngày: ${slots.join(', ')}</div>
    `;
    sentSchedulesEl.appendChild(div);
  });
}

// ── RENDER CALENDAR ───────────────────────────────────────
function renderCalendar() {
  const calendarEl = document.getElementById('calendar');
  const now = new Date();
  const year = now.getFullYear();
  const month = now.getMonth();

  // Tạo header tháng
  const monthNames = ['Tháng 1', 'Tháng 2', 'Tháng 3', 'Tháng 4', 'Tháng 5', 'Tháng 6', 'Tháng 7', 'Tháng 8', 'Tháng 9', 'Tháng 10', 'Tháng 11', 'Tháng 12'];
  const header = document.createElement('div');
  header.className = 'text-center mb-4';
  header.innerHTML = `<h3 class="text-lg font-semibold text-gray-800">${monthNames[month]} ${year}</h3>`;
  calendarEl.innerHTML = '';
  calendarEl.appendChild(header);

  // Tạo grid ngày
  const grid = document.createElement('div');
  grid.className = 'grid grid-cols-7 gap-1';

  // Header ngày trong tuần
  const daysOfWeek = ['CN', 'T2', 'T3', 'T4', 'T5', 'T6', 'T7'];
  daysOfWeek.forEach(day => {
    const dayEl = document.createElement('div');
    dayEl.className = 'text-center font-medium text-gray-600 py-2';
    dayEl.textContent = day;
    grid.appendChild(dayEl);
  });

  // Tìm ngày đầu tháng
  const firstDay = new Date(year, month, 1);
  const startDay = firstDay.getDay(); // 0 = CN

  // Thêm ô trống cho ngày trước
  for (let i = 0; i < startDay; i++) {
    const emptyEl = document.createElement('div');
    emptyEl.className = 'py-2';
    grid.appendChild(emptyEl);
  }

  // Thêm ngày trong tháng
  const daysInMonth = new Date(year, month + 1, 0).getDate();
  const daysWithSchedules = getDaysWithSchedules();

  for (let day = 1; day <= daysInMonth; day++) {
    const dayEl = document.createElement('div');
    const dateStr = `${year}-${String(month + 1).padStart(2, '0')}-${String(day).padStart(2, '0')}`;
    const hasSchedule = daysWithSchedules.includes(dateStr);

    dayEl.className = `text-center py-2 rounded-lg cursor-pointer transition-all duration-200 ${
      dateStr === state.selectedDate ? 'bg-green-500 text-white shadow-lg' :
      hasSchedule ? 'bg-blue-500 text-white shadow-md hover:bg-blue-600' : 'hover:bg-gray-200'
    }`;
    dayEl.innerHTML = `
      <div class="text-base font-semibold">${day}</div>
      ${hasSchedule ? '<span class="block mx-auto mt-1 h-1 w-1 rounded-full bg-red-500"></span>' : ''}
    `;
    if (hasSchedule) {
      dayEl.title = 'Có lịch uống thuốc';
    }
    dayEl.addEventListener('click', () => {
      state.selectedDate = dateStr;
      state.scheduleDate = dateStr;
      if (scheduleDateEl) scheduleDateEl.value = dateStr;
      renderDayDetails();
    });
    grid.appendChild(dayEl);
  }

  calendarEl.appendChild(grid);
}

function getDaysWithSchedules() {
  const days = new Set();
  state.sentSchedules.forEach(schedule => {
    const dateValue = schedule.date ? schedule.date : (schedule.timestamp ? new Date(schedule.timestamp).toISOString().split('T')[0] : null);
    if (dateValue) {
      days.add(dateValue);
    }
  });
  return Array.from(days);
}

function getLatestScheduleForDate(date) {
  const schedulesForDay = state.sentSchedules
    .filter(schedule => {
      const dateValue = schedule.date ? schedule.date : (schedule.timestamp ? new Date(schedule.timestamp).toISOString().split('T')[0] : null);
      return dateValue === date;
    })
    .sort((a, b) => {
      const ta = a.timestamp ? new Date(a.timestamp).getTime() : 0;
      const tb = b.timestamp ? new Date(b.timestamp).getTime() : 0;
      return tb - ta;
    });
  return schedulesForDay.length ? schedulesForDay[0] : null;
}

// ── RENDER DAY DETAILS ────────────────────────────────────
function renderDayDetails() {
  const detailsEl = document.getElementById('dayDetails');
  if (!state.selectedDate) {
    detailsEl.innerHTML = '<p class="text-gray-600">Chọn một ngày để xem chi tiết lịch uống thuốc.</p>';
    return;
  }

  const schedule = getLatestScheduleForDate(state.selectedDate);
  if (!schedule) {
    detailsEl.innerHTML = `<p class="text-gray-600">Ngày ${state.selectedDate} không có lịch uống thuốc.</p>`;
    return;
  }

  const dateObj = new Date(state.selectedDate);
  const dayNames = ['Chủ Nhật', 'Thứ Hai', 'Thứ Ba', 'Thứ Tư', 'Thứ Năm', 'Thứ Sáu', 'Thứ Bảy'];
  const formattedDate = `${dayNames[dateObj.getDay()]}, ${dateObj.getDate()}/${dateObj.getMonth() + 1}/${dateObj.getFullYear()}`;
  const slots = Array.isArray(schedule.slots) ? schedule.slots : [];

  const html = `
    <h4 class="text-lg font-semibold text-gray-800 mb-2">Lịch uống thuốc ngày ${formattedDate}</h4>
    <div class="bg-white p-3 rounded-lg shadow-sm">
      <div class="text-sm text-gray-600">Gửi lúc: ${schedule.time || '---'}</div>
      <div class="text-gray-800 font-medium">${schedule.times || slots.length} lần/ngày</div>
      <div class="text-blue-600">Thời gian: ${slots.join(', ')}</div>
    </div>
  `;

  detailsEl.innerHTML = html;
}

// ── LOAD SENT SCHEDULES FROM FIREBASE ─────────────────────
function loadSentSchedulesFromFirebase() {
  get(ordersRef)
    .then((snapshot) => {
      if (snapshot.exists()) {
        const data = snapshot.val();
        const schedules = Object.values(data)
          .map(safeSchedule)
          .filter(schedule => schedule.timestamp !== null)
          .sort((a, b) => new Date(b.timestamp) - new Date(a.timestamp));
        state.sentSchedules = schedules;
        renderSentSchedules();
        renderCalendar();
        renderDayDetails();
        addLog('Đã tải lịch từ Firebase ✓', 'ok');
      } else {
        renderCalendar();
        renderDayDetails();
        addLog('Không có lịch nào trong Firebase', 'warn');
      }
    })
    .catch((error) => {
      addLog(`Lỗi tải Firebase: ${error.message}`, 'error');
    });
}

// ── LOG ───────────────────────────────────────────────────
function addLog(msg, type = '') {
  // Xóa placeholder nếu còn
  const ph = logBox.querySelector('p');
  if (ph && ph.classList.contains('italic')) ph.remove();

  const now = new Date();
  const t   = `${String(now.getHours()).padStart(2,'0')}:${String(now.getMinutes()).padStart(2,'0')}:${String(now.getSeconds()).padStart(2,'0')}`;

  const entry = document.createElement('div');
  entry.className = 'flex space-x-2 text-sm py-1';
  let colorClass = '';
  if (type === 'ok') colorClass = 'text-green-600';
  else if (type === 'error') colorClass = 'text-red-600';
  else if (type === 'warn') colorClass = 'text-yellow-600';
  entry.innerHTML = `<span class="text-gray-500">${t}</span><span class="${colorClass}">${msg}</span>`;
  logBox.appendChild(entry);
  logBox.scrollTop = logBox.scrollHeight;
}

clearLogBtn.addEventListener('click', () => {
  logBox.innerHTML = '<p class="log-placeholder">Chưa có sự kiện nào...</p>';
});

// ── MQTT ─────────────────────────────────────────────────
function setMqttStatus(connected, label) {
  state.mqttConnected = connected;
  const dot = document.getElementById('mqttDot');
  const lbl = document.getElementById('mqttLabel');
  if (connected) {
    dot.className = 'w-4 h-4 bg-green-500 rounded-full shadow-md animate-pulse';
    lbl.className = 'text-sm font-medium text-green-600';
  } else {
    dot.className = 'w-4 h-4 bg-red-500 rounded-full shadow-md';
    lbl.className = 'text-sm font-medium text-red-600';
  }
  lbl.textContent = label;
}

function initMqtt() {
  client = new Paho.Client(MQTT_HOST, MQTT_PORT, CLIENT_ID);

  client.onConnectionLost = (resp) => {
    setMqttStatus(false, 'Mất kết nối');
    addLog(`MQTT mất kết nối: ${resp.errorMessage}`, 'error');
    setTimeout(connectMqtt, 5000);
  };

  client.onMessageArrived = (msg) => {
    const topic   = msg.destinationName;
    const payload = msg.payloadString.trim();
    handleMqttMessage(topic, payload);
  };

  connectMqtt();
}

function connectMqtt() {
  mqttDot.className = 'dot dot--off';
  mqttLabel.textContent = 'Đang kết nối...';
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

  TOPICS_STATUS.forEach((topic, i) => {
    client.subscribe(topic);
    addLog(`Đăng ký topic: ${topic}`);
  });

  // Cũng lắng nghe phản hồi lịch từ ESP (nếu có)
  client.subscribe('esp/schedule/ack');
}

function onConnectFail(resp) {
  setMqttStatus(false, 'Kết nối thất bại');
  addLog(`Kết nối MQTT thất bại (${resp.errorMessage}) – thử lại sau 5s`, 'error');
  setTimeout(connectMqtt, 5000);
}

function handleMqttMessage(topic, payload) {
  // Trạng thái hộp thuốc: esp/h1 .. esp/h4
  const boxIdx = TOPICS_STATUS.indexOf(topic);
  if (boxIdx !== -1) {
    const hasmed = payload === '1';
    const box    = state.boxes[boxIdx];
    const prev   = box.status;
    box.status    = hasmed ? 'has' : 'empty';
    box.updatedAt = fmtTime();

    // Cập nhật card cụ thể (không render lại toàn bộ)
    updateBoxCard(boxIdx);

    if (prev !== box.status) {
      const label = box.label;
      const msg   = hasmed
        ? `${label}: còn thuốc ✓`
        : `${label}: hết thuốc ⚠`;
      addLog(msg, hasmed ? 'ok' : 'warn');
    }
    return;
  }

  // ACK từ ESP32 sau khi nhận lịch
  if (topic === 'esp/schedule/ack') {
    addLog(`ESP32 xác nhận lịch: ${payload}`, 'ok');
    return;
  }

  addLog(`[${topic}] ${payload}`);
}

function updateBoxCard(idx) {
  const cards = boxGrid.querySelectorAll('.box-card');
  if (!cards[idx]) {
    renderBoxes();
    return;
  }
  const box  = state.boxes[idx];
  const card = cards[idx];
  const cls  = box.status === 'has' ? 'has'
             : box.status === 'empty' ? 'empty' : 'unknown';
  card.className = `box-card ${cls}`;

  const icon   = box.status === 'has' ? '💊'
               : box.status === 'empty' ? '🫙' : '❓';
  const status = box.status === 'has' ? 'Còn thuốc'
               : box.status === 'empty' ? 'Hết thuốc' : 'Chưa có dữ liệu';
  const updated = `Cập nhật: ${box.updatedAt}`;

  card.querySelector('.box-icon').textContent   = icon;
  card.querySelector('.box-status').textContent = status;
  card.querySelector('.box-updated').textContent = updated;
}

function fmtTime() {
  const d = new Date();
  return `${String(d.getHours()).padStart(2,'0')}:${String(d.getMinutes()).padStart(2,'0')}:${String(d.getSeconds()).padStart(2,'0')}`;
}

// ── INIT ──────────────────────────────────────────────────
renderBoxes();
renderTimeSlots();
applyDatePicker();
renderSentSchedules();
renderCalendar();
renderDayDetails();
loadSentSchedulesFromFirebase();
initMqtt();