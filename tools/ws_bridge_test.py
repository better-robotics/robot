#!/usr/bin/env python3
"""Validate the ESP32 WS<->TCP bridge with a real MQTT-over-WebSocket client
(the browser transport). Connects ws://192.168.4.1:9001/ -> bridge -> broker."""
import sys, time
import paho.mqtt.client as mqtt

HOST, PORT = "192.168.4.1", 9001

def newc(user, pw):
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, transport="websockets")
    c.ws_set_options(path="/")
    c.username_pw_set(user, pw)
    return c

def test_reject():
    c = newc("team1", "WRONG")
    st = {}
    c.on_connect = lambda cl, u, f, rc, p: st.__setitem__("rc", getattr(rc, "value", rc))
    try:
        c.connect(HOST, PORT, 10)
    except Exception as e:
        print("  reject: connect raised", e); return False
    c.loop_start(); time.sleep(4); c.loop_stop()
    try: c.disconnect()
    except Exception: pass
    rc = st.get("rc")
    print(f"  reject: CONNACK reason_code value={rc} (nonzero = auth denied through bridge)")
    return rc not in (0, None)

def test_roundtrip():
    c = newc("professor", "change-me")
    got = {}
    def on_connect(cl, u, f, rc, p):
        got["conn"] = getattr(rc, "value", rc)
        if got["conn"] == 0:
            cl.subscribe("brobo/#")
    c.on_connect = on_connect
    c.on_subscribe = lambda cl, u, mid, rc, p: cl.publish("brobo/hi", "through-the-bridge", qos=1)
    c.on_message = lambda cl, u, m: got.__setitem__("msg", (m.topic, m.payload.decode()))
    c.connect(HOST, PORT, 10)
    c.loop_start()
    end = time.time() + 8
    while time.time() < end and "msg" not in got:
        time.sleep(0.1)
    c.loop_stop()
    try: c.disconnect()
    except Exception: pass
    print(f"  roundtrip: connect rc={got.get('conn')}  received={got.get('msg')}")
    return got.get("conn") == 0 and got.get("msg") is not None

print("== WS-bridge test (browser MQTT-over-WebSocket transport) ==")
r = test_reject()
g = test_roundtrip()
print("REJECT_OK" if r else "REJECT_FAIL")
print("ROUNDTRIP_OK" if g else "ROUNDTRIP_FAIL")
sys.exit(0 if (r and g) else 1)
