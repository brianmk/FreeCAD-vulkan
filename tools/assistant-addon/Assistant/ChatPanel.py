"""Assistant - the VSCode-style chat dock widget.

A QDockWidget hosting a scrollable message area (Markdown + tool cards), an
input box, and transport controls.  It owns the Agent and renders its signals.
"""

import base64
import html
import mimetypes
import os
import re

from PySide import QtCore, QtGui, QtWidgets

import Preferences as P
from Agent import Agent

MODULE_DIR = os.path.dirname(os.path.abspath(__file__))


class ChatPanel(QtWidgets.QDockWidget):
    def __init__(self, parent=None):
        title = "Assistant"
        super().__init__(title, parent)
        self.setObjectName("AssistantChat")
        self.setAllowedAreas(QtCore.Qt.LeftDockWidgetArea | QtCore.Qt.RightDockWidgetArea)
        # Locked by default: docked only, no float / no drag. (Stays closeable.)
        self.setFeatures(QtWidgets.QDockWidget.DockWidgetClosable)

        self._agent = Agent()
        self._blocks = []            # list of html strings (committed)
        self._stream = ""            # live streamed assistant text
        self._tool_card_ix = None    # blocks index of the in-progress tool card

        w = QtWidgets.QWidget(self)
        lay = QtWidgets.QVBoxLayout(w)
        lay.setContentsMargins(4, 4, 4, 4)

        # transport row
        self._model = QtWidgets.QComboBox()
        self._model.setEditable(True)
        self._model.addItem(P.model())
        self._model.setToolTip("Model id")
        self._mode = QtWidgets.QComboBox()
        for label, val in (("Approve each tool", P.MODE_APPROVE),
                           ("Auto-run", P.MODE_AUTO), ("Dry-run", P.MODE_DRY)):
            self._mode.addItem(label, val)
        self._mode.setCurrentIndex(self._mode.findData(P.mode()))
        if self._mode.currentIndex() < 0:
            self._mode.setCurrentIndex(0)

        top = QtWidgets.QHBoxLayout()
        top.addWidget(QtWidgets.QLabel("Model"))
        top.addWidget(self._model, 1)
        top.addWidget(QtWidgets.QLabel("Mode"))
        top.addWidget(self._mode)
        lay.addLayout(top)

        # camera controls (fit / standard views / rotate / zoom)
        cam = QtWidgets.QHBoxLayout()
        cam.addWidget(QtWidgets.QLabel("Camera"))
        for text, action in (("Fit", "fit"), ("Iso", "isometric"), ("Top", "top"),
                             ("Front", "front"), ("Right", "right"),
                             ("⟲", "rotate_left"), ("⟳", "rotate_right"),
                             ("＋", "zoom_in"), ("－", "zoom_out")):
            b = QtWidgets.QToolButton()
            b.setText(text); b.setAutoRaise(True)
            b.setToolTip(action.replace("_", " "))
            b.clicked.connect(lambda _=False, a=action: self._camera(a))
            cam.addWidget(b)
        cam.addStretch(1)
        lay.addLayout(cam)

        # context-aware quick-start suggestions (new document / new sketch links)
        self._sugg = QtWidgets.QWidget(w)
        self._sugg_lay = QtWidgets.QHBoxLayout(self._sugg)
        self._sugg_lay.setContentsMargins(0, 0, 0, 0)
        self._sugg_lay.setSpacing(8)
        self._sugg.setVisible(False)
        lay.addWidget(self._sugg)

        # message browser
        self._view = QtWidgets.QTextBrowser()
        self._view.setOpenExternalLinks(True)
        lay.addWidget(self._view, 1)

        # input row
        self._input = QtWidgets.QPlainTextEdit()
        self._input.setPlaceholderText("Ask the assistant... (Enter to send, Shift+Enter for newline)")
        self._input.setMaximumHeight(96)
        lay.addWidget(self._input)

        bottom = QtWidgets.QHBoxLayout()
        self._cb_view = QtWidgets.QCheckBox("View")
        self._cb_view.setToolTip("Attach a snapshot of the current 3D view to the message (vision model)")
        self._picture = QtWidgets.QPushButton("🖼")
        self._picture.setToolTip("Attach a picture from disk (drag/drop or paste also work)")
        self._picture.clicked.connect(self._pick_image)
        self._cb_draw = QtWidgets.QCheckBox("Draw")
        self._cb_draw.setToolTip("Reconstruct the attached picture as a sketch (sketch_* + add_constraint)")
        self._send = QtWidgets.QPushButton("Send")
        self._stop = QtWidgets.QPushButton("Stop")
        self._stop.setEnabled(False)
        self._clear = QtWidgets.QPushButton("Clear")
        self._settings = QtWidgets.QPushButton("Settings")
        bottom.addWidget(self._cb_view)
        bottom.addWidget(self._picture)
        bottom.addWidget(self._cb_draw)
        bottom.addWidget(self._send)
        bottom.addWidget(self._stop)
        bottom.addWidget(self._clear)
        bottom.addWidget(self._settings)
        bottom.addStretch(1)
        self._usage = QtWidgets.QLabel("")
        self._usage.setStyleSheet("color:#666;font-size:90%")
        bottom.addWidget(self._usage)
        lay.addLayout(bottom)

        self.setWidget(w)

        # wiring
        self._send.clicked.connect(self._on_send)
        self._clear.clicked.connect(self.clear_conversation)
        self._stop.clicked.connect(self._on_stop)
        self._settings.clicked.connect(self._open_settings)
        self._model.currentTextChanged.connect(lambda t: P.set("Model", t))
        self._mode.currentIndexChanged.connect(lambda i: P.set("Mode", self._mode.itemData(i)))
        self._input.installEventFilter(self)
        self._agent.text_delta.connect(self._on_text_delta)
        self._agent.tool_call.connect(self._on_tool_call)
        self._agent.approval.connect(self._on_approval)
        self._agent.status.connect(self._on_status)
        self._agent.usage.connect(self._on_usage)
        self._agent.finished.connect(self._on_finished)
        self._agent.failed.connect(self._on_failed)

        self._pending_image = None   # (data_url, mime, name) for an externally attached picture
        self._input.setAcceptDrops(True)

        self._render()
        self._refresh_suggestions()
        self.setMinimumWidth(380)
        self.resize(460, 640)

    # ---- input helpers -----------------------------------------------------
    def focus_input(self):
        self._input.setFocus()

    def eventFilter(self, obj, ev):
        if obj is self._input:
            t = ev.type()
            if t == QtCore.QEvent.KeyPress:
                if ev.key() in (QtCore.Qt.Key_Return, QtCore.Qt.Key_Enter) and not (
                        ev.modifiers() & QtCore.Qt.ShiftModifier):
                    self._on_send()
                    return True
                if ev.matches(QtGui.QKeySequence.Paste):
                    self._paste_image()
            elif t == QtCore.QEvent.DragEnter:
                self._drag_enter(ev)
                return True
            elif t == QtCore.QEvent.Drop:
                self._drop(ev)
                return True
        return super().eventFilter(obj, ev)

    # ---- image attachment --------------------------------------------------
    def _pick_image(self):
        path, _ = QtWidgets.QFileDialog.getOpenFileName(
            self, "Attach picture", "", "Images (*.png *.jpg *.jpeg *.bmp *.webp *.gif)")
        if path:
            self._set_image_file(path)

    def _set_image_file(self, path):
        if not os.path.isfile(path):
            self._status(f"🖼 not a file: {path}")
            return
        try:
            with open(path, "rb") as f:
                data = base64.b64encode(f.read()).decode("ascii")
        except Exception as exc:
            self._status(f"🖼 read failed: {exc}")
            return
        mime = mimetypes.guess_type(path)[0] or "image/png"
        name = os.path.basename(path)
        self._attach_image_data(f"data:{mime};base64,{data}", mime, name)

    def _attach_image_data(self, data_url, mime, name):
        self._pending_image = (data_url, mime, name)
        self._cb_view.setChecked(False)
        self._cb_draw.setChecked(True)
        self._blocks.append(
            f"<div style='color:gray;font-size:90%;margin:2px 0'>🖼 picture attached: {html.escape(name)}</div>")
        self._render()

    def _clear_attached_image(self):
        self._pending_image = None

    def _drag_enter(self, ev):
        if ev.mimeData().hasUrls() or ev.mimeData().hasImage():
            ev.acceptProposedAction()

    def _drop(self, ev):
        md = ev.mimeData()
        if md.hasUrls() and md.urls():
            self._set_image_file(md.urls()[0].toLocalFile())
        elif md.hasImage():
            img = QtGui.QImage(md.imageData())
            self._attach_image_data(self._qimage_to_dataurl(img), "image/png", "pasted.png")

    def _paste_image(self):
        cb = QtWidgets.QApplication.clipboard()
        if cb.mimeData().hasImage():
            self._attach_image_data(self._qimage_to_dataurl(QtGui.QImage(cb.image())),
                                    "image/png", "clipboard.png")
            return True
        return False

    @staticmethod
    def _qimage_to_dataurl(img):
        buf = QtCore.QBuffer()
        buf.open(QtCore.QIODevice.WriteOnly)
        img.save(buf, "PNG")
        return "data:image/png;base64," + base64.b64encode(buf.data().data()).decode("ascii")

    def _on_send(self):
        text = self._input.toPlainText().strip()
        if not text:
            return
        self._input.clear()
        self._refresh_suggestions()
        self._push_user(text)
        self._stream = ""
        image_url = image_mime = None
        draw = self._cb_draw.isChecked()
        if self._pending_image:
            image_url, image_mime, _ = self._pending_image
        elif self._cb_view.isChecked():
            from Context import capture_snapshot
            snap = capture_snapshot()
            if snap:
                image_url, image_mime = snap
                self._blocks.append("<div style='color:gray;font-size:90%;margin:2px 0'>📷 view snapshot attached</div>")
            else:
                self._blocks.append("<div style='color:#c00'>📷 no active 3D view to capture</div>")
        if image_url:
            draw = draw or self._pending_image is not None
            self._cb_draw.setChecked(draw)
        self._send.setEnabled(False)
        self._stop.setEnabled(True)
        self._status("thinking...")
        self._render()
        self._agent.send(text, image_url=image_url, image_mime=image_mime, draw=draw)
        self._pending_image = None

    def _on_stop(self):
        self._agent.stop()
        self._stop.setEnabled(False)
        self._send.setEnabled(True)

    def _camera(self, action):
        import ToolRegistry as R
        try:
            R.call("control_camera", {"action": action})
        except Exception as exc:
            self._status(f"camera {action}: {exc}")

    def _refresh_suggestions(self):
        from Context import build_suggestions
        while self._sugg_lay.count():
            item = self._sugg_lay.takeAt(0)
            wid = item.widget()
            if wid is not None:
                wid.deleteLater()
        items = build_suggestions()
        if not items:
            self._sugg.setVisible(False)
            return
        for s in items:
            btn = QtWidgets.QPushButton(s.get("label", s["command"]) + " →")
            btn.setCursor(QtGui.QCursor(QtCore.Qt.PointingHandCursor))
            btn.setFlat(True)
            btn.setStyleSheet(
                "QPushButton{border:none;background:transparent;color:#3b82f6;"
                "text-decoration:underline;padding:0 2px;font-weight:600}"
                "QPushButton:hover{color:#1d4ed8}"
                "QPushButton:disabled{color:#888}")
            btn.setToolTip(s.get("hint", ""))
            btn.clicked.connect(lambda _=False, s=s: self._run_suggestion(s))
            self._sugg_lay.addWidget(btn)
        self._sugg_lay.addStretch(1)
        self._sugg.setVisible(True)

    def _run_suggestion(self, s):
        import ToolRegistry as R
        self._status(f"⚙ {s.get('label', s['command'])}...")
        try:
            res = R.call(s["command"], s.get("args") or {})
            self._status(f"✔ done: {res}")
        except Exception as exc:
            self._status(f"✖ {s.get('label', s['command'])}: {exc}")
        finally:
            self._refresh_suggestions()

    def _open_settings(self):
        import Preferences as P
        dlg = QtWidgets.QDialog(self)
        dlg.setWindowTitle("Assistant Settings")
        form = QtWidgets.QFormLayout(dlg)
        endpoint = QtWidgets.QLineEdit(P.endpoint())
        model = QtWidgets.QLineEdit(P.model())
        vision_model_edit = QtWidgets.QLineEdit(P.vision_model())
        key = QtWidgets.QLineEdit(P.api_key())
        key.setEchoMode(QtWidgets.QLineEdit.Password)
        mode = QtWidgets.QComboBox()
        for label, val in (("Approve each tool", P.MODE_APPROVE),
                           ("Auto-run", P.MODE_AUTO), ("Dry-run", P.MODE_DRY)):
            mode.addItem(label, val)
        mode.setCurrentIndex(mode.findData(P.mode()))
        max_turns = QtWidgets.QSpinBox(); max_turns.setRange(1, 50); max_turns.setValue(P.max_turns())
        vision_check = QtWidgets.QCheckBox("Attach viewport snapshot (vision model)")
        vision_check.setChecked(P.vision_enabled())
        debug_check = QtWidgets.QCheckBox("Debug (show tool args & results)")
        debug_check.setChecked(P.debug())
        form.addRow("Endpoint", endpoint)
        form.addRow("Model", model)
        form.addRow("Vision model", vision_model_edit)
        form.addRow("API key", key)
        form.addRow("Tool mode", mode)
        form.addRow("Max turns", max_turns)
        form.addRow("", vision_check)
        form.addRow("", debug_check)
        hint = QtWidgets.QLabel("API key also auto-read from the DEEPSEEK_API_KEY env var.")
        hint.setWordWrap(True)
        form.addRow(hint)

        buttons = QtWidgets.QDialogButtonBox(QtWidgets.QDialogButtonBox.Ok
                                             | QtWidgets.QDialogButtonBox.Cancel)
        buttons.accepted.connect(dlg.accept)
        buttons.rejected.connect(dlg.reject)
        form.addRow(buttons)

        if dlg.exec():
            P.set("Endpoint", endpoint.text().strip())
            P.set("Model", model.text().strip())
            P.set("VisionModel", vision_model_edit.text().strip())
            P.set_api_key(key.text())
            P.set("Mode", mode.itemData(mode.currentIndex()))
            P.set("MaxTurns", max_turns.value())
            P.set("VisionEnabled", vision_check.isChecked())
            P.set("Debug", debug_check.isChecked())
            self._model.setCurrentText(P.model())
            self._mode.setCurrentIndex(self._mode.findData(P.mode()))
            self._cb_view.setChecked(vision_check.isChecked())
            self._status("settings saved")

    def clear_conversation(self):
        self._blocks = []
        self._stream = ""
        self._tool_card_ix = None
        self._agent.clear_history()
        self._pending_image = None
        self._render()

    # ---- agent signals -----------------------------------------------------
    def _on_text_delta(self, delta):
        self._stream += delta
        self._render()

    def _card_html(self, info):
        name = html.escape(str(info.get("name")))
        status = info.get("status", "pending")
        status_icon = {"pending": "…", "running": "⏳", "ran": "✔", "rejected": "✖",
                       "dry": "⏭", "error": "⚠"}.get(status, "•")
        card = (f"<div style=\"border:1px solid #888;border-radius:6px;padding:6px;"
                f"margin:6px 0;background:rgba(128,128,128,0.08)\">"
                f"<b>{status_icon} {name}</b> <span style='color:#888'>({status})</span>")
        if P.debug():
            args = html.escape(json_dumps(info.get("arguments", {})))
            card += (f"<br><details><summary>args</summary>"
                     f"<pre style='white-space:pre-wrap'>{args}</pre></details>")
            result = info.get("result", "")
            if result:
                esc = html.escape(str(result))
                card += (f"<details><summary>result</summary>"
                         f"<pre style='white-space:pre-wrap;max-height:180px;overflow:auto'>{esc}</pre></details>")
        card += "</div>"
        return card

    def _on_tool_call(self, info):
        """Render a SINGLE card per tool call, updating it in place as the status
        moves pending -> running -> ran/error (no duplicate cards)."""
        status = info.get("status", "pending")
        if status == "pending":
            if self._stream:
                self._blocks.append(md_to_html(self._stream))
            self._stream = ""
            self._blocks.append(self._card_html(info))
            self._tool_card_ix = len(self._blocks) - 1
        else:
            # update the existing card for this (sequential) tool call
            if self._tool_card_ix is not None and 0 <= self._tool_card_ix < len(self._blocks):
                self._blocks[self._tool_card_ix] = self._card_html(info)
            else:
                self._blocks.append(self._card_html(info))
                self._tool_card_ix = len(self._blocks) - 1
        self._render()

    def _on_approval(self, info):
        name = info.get("name", "?")
        box = QtWidgets.QMessageBox(self)
        box.setWindowTitle("Approval required")
        box.setIcon(QtWidgets.QMessageBox.Question)
        box.setText("Allow the assistant to run this tool?")
        info_text = f"<b>{html.escape(str(name))}</b>"
        if P.debug():
            args = json_dumps(info.get("arguments", {}))
            info_text += f"\n{html.escape(args)}"
        box.setInformativeText(info_text)
        approve = box.addButton("Approve", QtWidgets.QMessageBox.AcceptRole)
        reject = box.addButton("Reject", QtWidgets.QMessageBox.RejectRole)
        box.setDefaultButton(approve)
        box.exec()
        self._agent.set_approval(info.get("token"), box.clickedButton() is approve)

    def _on_status(self, msg):
        self._status(msg)

    def _on_finished(self, text):
        self._blocks.append(md_to_html(self._stream or text))
        self._stream = ""
        self._send.setEnabled(True)
        self._stop.setEnabled(False)
        self._render()
        self._refresh_suggestions()

    def _on_failed(self, msg):
        self._blocks.append(f"<div style='color:#c00'>Error: {html.escape(str(msg))}</div>")
        self._stream = ""
        self._send.setEnabled(True)
        self._stop.setEnabled(False)
        self._render()
        self._refresh_suggestions()

    def _on_usage(self, u):
        p = u.get("prompt_tokens", "?")
        c = u.get("completion_tokens", "?")
        t = u.get("total_tokens", "?")
        self._usage.setText(f"tokens: prompt {p} · completion {c} · total {t}")

    # ---- rendering ---------------------------------------------------------
    def _render(self):
        out = ["<div style='font-family:inherit'>"]
        for b in self._blocks:
            out.append(b)
        if self._stream:
            out.append(md_to_html(self._stream))
        out.append("</div>")
        self._view.setHtml("".join(out))
        sb = self._view.verticalScrollBar()
        sb.setValue(sb.maximum())

    def _push_user(self, text):
        self._blocks.append(
            f"<div style='color:#3b82f6;font-weight:600;margin:6px 0'>🧑 {html.escape(text)}</div>")

    def _status(self, msg):
        self._blocks.append(
            f"<div style='color:gray;font-size:90%;margin:2px 0'>{html.escape(msg)}</div>")
        self._render()


def md_to_html(md_text):
    from PySide import QtGui
    doc = QtGui.QTextDocument()
    doc.setMarkdown(md_text or "(no response)")
    h = doc.toHtml()
    m = re.search(r"<body[^>]*>(.*)</body>", h, re.S)
    if m:
        return m.group(1)
    # Fallback if QTextDocument produced no <body> wrapper: strip wrappers.
    h2 = re.sub(r"</?(?:html|head|body|meta|title)[^>]*>", "", h, flags=re.S)
    return h2 or "<p></p>"


def json_dumps(obj):
    import json
    try:
        return json.dumps(obj, default=str)
    except Exception:
        return str(obj)
