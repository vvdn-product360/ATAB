const express = require('express');
const app = express();
const PORT = 3000;

app.use(express.json());
app.use(require('cors')());

let notifications = [
  { id: "NOTIF_001", message: "Boarding Now - Gate B14" }
];

// GET notifications
app.get('/devices/:device_id/notifications', (req, res) => {
  res.json({
    device_id: req.params.device_id,
    notifications: notifications
  });
});

// POST new alert
app.post('/send-alert', (req, res) => {
  const { message } = req.body;

  const newNotif = {
    id: "NOTIF_" + Date.now(),
    message: message
  };

  notifications.push(newNotif);

  res.json({ success: true, notification: newNotif });
});

app.listen(PORT, () => {
  console.log(`Server running on http://localhost:${PORT}`);
});