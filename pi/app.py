from flask import Flask, jsonify, send_from_directory, abort
import json
import os

app = Flask(__name__)

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
FIRMWARE_DIR = os.path.join(BASE_DIR, 'firmware')
METADATA_FILE = os.path.join(BASE_DIR, 'metadata.json')

@app.route('/firmware/latest')
def latest_firmware():
    if not os.path.exists(METADATA_FILE):
        return jsonify({'error': 'no firmware available'}), 404

    with open(METADATA_FILE, 'r') as f:
        metadata = json.load(f)

    return jsonify(metadata)

@app.route('/firmware/download/<filename>')
def download_firmware(filename):
    file_path = os.path.join(FIRMWARE_DIR, filename)
    if not os.path.exists(file_path):
        abort(404)

    return send_from_directory(FIRMWARE_DIR, filename, mimetype='application/octet-stream')

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000)