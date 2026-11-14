#!/usr/bin/env python3
import sys
import json
import time
from paho.mqtt import client as mqtt_client

broker = '127.0.0.1'
port = 1883
topic = 'desk/testsrc2_detect'
client_id = 'mqtt-capture-script'
output_file = sys.argv[1] if len(sys.argv) > 1 else 'mqtt_captured.json'

messages = []

def connect_mqtt():
    def on_connect(client, userdata, flags, rc, properties=None):
        if rc == 0:
            print("Connected to MQTT Broker!")
        else:
            print(f"Failed to connect, return code {rc}\n")
    client = mqtt_client.Client(client_id=client_id, callback_api_version=mqtt_client.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.connect(broker, port)
    return client

def subscribe(client: mqtt_client.Client):
    def on_message(client, userdata, msg):
        payload_str = msg.payload.decode()
        print(f"Received msg: {payload_str[:200]}...")
        try:
            payload = json.loads(payload_str)
            messages.append(payload)
        except Exception as e:
            print(f"Error parsing message: {e}")

    client.subscribe(topic)
    client.on_message = on_message

def main():
    client = connect_mqtt()
    subscribe(client)
    client.loop_start()

    start_time = time.time()
    # Let it run for up to 75 seconds
    while time.time() - start_time < 75:
        time.sleep(1)
        if len(messages) >= 5:
            pass

    client.loop_stop()
    client.disconnect()

    with open(output_file, 'w') as f:
        json.dump(messages, f, indent=2)
    print(f"Saved {len(messages)} messages to {output_file}")

if __name__ == '__main__':
    main()
