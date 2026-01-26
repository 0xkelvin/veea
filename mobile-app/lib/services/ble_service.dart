import 'dart:async';
import 'dart:typed_data';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

/// UUIDs for the Veea Camera BLE service
class VeeaUuids {
  // Custom service UUID for image transfer
  static const String imageService = "12345678-1234-5678-1234-56789abcdef0";
  // Characteristic to receive image data (chunked)
  static const String imageDataChar = "12345678-1234-5678-1234-56789abcdef1";
  // Characteristic to receive image metadata (size, format)
  static const String imageMetaChar = "12345678-1234-5678-1234-56789abcdef2";
  // Characteristic to trigger capture
  static const String captureChar = "12345678-1234-5678-1234-56789abcdef3";
}

/// Image metadata received from device
class ImageMetadata {
  final int width;
  final int height;
  final int size;
  final String format;
  final DateTime timestamp;

  ImageMetadata({
    required this.width,
    required this.height,
    required this.size,
    required this.format,
    DateTime? timestamp,
  }) : timestamp = timestamp ?? DateTime.now();

  factory ImageMetadata.fromBytes(List<int> bytes) {
    if (bytes.length < 12) {
      throw Exception('Invalid metadata length');
    }
    // Format: width(2) + height(2) + size(4) + format(4)
    final width = bytes[0] | (bytes[1] << 8);
    final height = bytes[2] | (bytes[3] << 8);
    final size = bytes[4] | (bytes[5] << 8) | (bytes[6] << 16) | (bytes[7] << 24);
    final format = String.fromCharCodes(bytes.sublist(8, 12));
    return ImageMetadata(
      width: width,
      height: height,
      size: size,
      format: format.trim(),
    );
  }
}

/// BLE Service for communicating with Veea Camera device
class BleService {
  static final BleService _instance = BleService._internal();
  factory BleService() => _instance;
  BleService._internal();

  BluetoothDevice? _connectedDevice;
  BluetoothCharacteristic? _imageDataChar;
  BluetoothCharacteristic? _imageMetaChar;
  BluetoothCharacteristic? _captureChar;

  final _connectionStateController = StreamController<bool>.broadcast();
  final _scanResultsController = StreamController<List<ScanResult>>.broadcast();
  final _imageReceivedController = StreamController<Uint8List>.broadcast();
  final _metadataController = StreamController<ImageMetadata>.broadcast();

  Stream<bool> get connectionState => _connectionStateController.stream;
  Stream<List<ScanResult>> get scanResults => _scanResultsController.stream;
  Stream<Uint8List> get imageReceived => _imageReceivedController.stream;
  Stream<ImageMetadata> get metadataReceived => _metadataController.stream;

  bool get isConnected => _connectedDevice != null;
  BluetoothDevice? get connectedDevice => _connectedDevice;

  // Image transfer state
  ImageMetadata? _currentMetadata;
  List<int> _imageBuffer = [];
  int _receivedBytes = 0;

  StreamSubscription? _scanSubscription;

  /// Start scanning for Veea devices
  Future<void> startScan({Duration timeout = const Duration(seconds: 10)}) async {
    // Cancel any existing scan subscription
    await _scanSubscription?.cancel();
    
    final results = <ScanResult>[];

    // Listen to scan results
    _scanSubscription = FlutterBluePlus.scanResults.listen((scanResults) {
      results.clear();
      for (var result in scanResults) {
        // Filter for veea-device by name
        if (result.device.platformName.toLowerCase().contains('veea')) {
          results.add(result);
        }
      }
      _scanResultsController.add(List.from(results));
    });

    try {
      await FlutterBluePlus.startScan(timeout: timeout);
    } catch (e) {
      print('Scan error: $e');
    }
  }

  /// Stop scanning
  Future<void> stopScan() async {
    try {
      await _scanSubscription?.cancel();
      _scanSubscription = null;
      await FlutterBluePlus.stopScan();
    } catch (e) {
      print('Stop scan error: $e');
    }
  }

  /// Connect to a device
  Future<void> connect(BluetoothDevice device) async {
    try {
      await device.connect(timeout: const Duration(seconds: 10));
      _connectedDevice = device;
      _connectionStateController.add(true);

      // Discover services
      final services = await device.discoverServices();
      await _setupCharacteristics(services);

      // Listen for disconnection
      device.connectionState.listen((state) {
        if (state == BluetoothConnectionState.disconnected) {
          _handleDisconnection();
        }
      });
    } catch (e) {
      _connectionStateController.add(false);
      rethrow;
    }
  }

  /// Disconnect from device
  Future<void> disconnect() async {
    await _connectedDevice?.disconnect();
    _handleDisconnection();
  }

  void _handleDisconnection() {
    _connectedDevice = null;
    _imageDataChar = null;
    _imageMetaChar = null;
    _captureChar = null;
    _connectionStateController.add(false);
  }

  /// Setup characteristics and notifications
  Future<void> _setupCharacteristics(List<BluetoothService> services) async {
    for (var service in services) {
      if (service.uuid.toString().toLowerCase() == VeeaUuids.imageService.toLowerCase()) {
        for (var char in service.characteristics) {
          final uuid = char.uuid.toString().toLowerCase();
          if (uuid == VeeaUuids.imageDataChar.toLowerCase()) {
            _imageDataChar = char;
            await char.setNotifyValue(true);
            char.onValueReceived.listen(_onImageDataReceived);
          } else if (uuid == VeeaUuids.imageMetaChar.toLowerCase()) {
            _imageMetaChar = char;
            await char.setNotifyValue(true);
            char.onValueReceived.listen(_onMetadataReceived);
          } else if (uuid == VeeaUuids.captureChar.toLowerCase()) {
            _captureChar = char;
          }
        }
      }
    }
  }

  /// Handle received image metadata
  void _onMetadataReceived(List<int> value) {
    try {
      _currentMetadata = ImageMetadata.fromBytes(value);
      _imageBuffer = [];
      _receivedBytes = 0;
      _metadataController.add(_currentMetadata!);
    } catch (e) {
      print('Error parsing metadata: $e');
    }
  }

  /// Handle received image data chunk
  void _onImageDataReceived(List<int> value) {
    if (_currentMetadata == null) return;

    _imageBuffer.addAll(value);
    _receivedBytes += value.length;

    // Check if we have received the complete image
    if (_receivedBytes >= _currentMetadata!.size) {
      final imageData = Uint8List.fromList(_imageBuffer.take(_currentMetadata!.size).toList());
      _imageReceivedController.add(imageData);
      
      // Reset state
      _currentMetadata = null;
      _imageBuffer = [];
      _receivedBytes = 0;
    }
  }

  /// Trigger a capture on the device
  Future<void> triggerCapture() async {
    if (_captureChar == null) {
      throw Exception('Not connected or capture characteristic not found');
    }
    // Send capture command (0x01 = capture)
    await _captureChar!.write([0x01], withoutResponse: false);
  }

  /// Request MTU size for faster transfer
  Future<int> requestMtu(int mtu) async {
    if (_connectedDevice == null) return 23;
    return await _connectedDevice!.requestMtu(mtu);
  }

  void dispose() {
    _scanSubscription?.cancel();
    _connectionStateController.close();
    _scanResultsController.close();
    _imageReceivedController.close();
    _metadataController.close();
  }
}
