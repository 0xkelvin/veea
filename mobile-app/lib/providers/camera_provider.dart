import 'dart:async';
import 'dart:io';
import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import '../services/ble_service.dart';
import '../services/image_service.dart';
import '../models/captured_image.dart';

enum ConnectionStatus { disconnected, scanning, connecting, connected }

class CameraProvider extends ChangeNotifier {
  final BleService _bleService = BleService();
  final ImageService _imageService = ImageService();

  ConnectionStatus _connectionStatus = ConnectionStatus.disconnected;
  List<ScanResult> _scanResults = [];
  String? _errorMessage;
  bool _isCapturing = false;
  double _transferProgress = 0.0;

  StreamSubscription? _connectionSub;
  StreamSubscription? _scanSub;
  StreamSubscription? _imageSub;
  StreamSubscription? _metaSub;

  ConnectionStatus get connectionStatus => _connectionStatus;
  List<ScanResult> get scanResults => _scanResults;
  List<CapturedImage> get images => _imageService.images;
  String? get errorMessage => _errorMessage;
  bool get isCapturing => _isCapturing;
  double get transferProgress => _transferProgress;
  bool get isConnected => _connectionStatus == ConnectionStatus.connected;

  CameraProvider() {
    _initAsync();
  }

  void _initAsync() {
    // Setup BLE listeners first (non-blocking)
    _connectionSub = _bleService.connectionState.listen((connected) {
      _connectionStatus = connected ? ConnectionStatus.connected : ConnectionStatus.disconnected;
      notifyListeners();
    });

    _scanSub = _bleService.scanResults.listen((results) {
      _scanResults = results;
      notifyListeners();
    });

    _metaSub = _bleService.metadataReceived.listen((meta) {
      _isCapturing = true;
      _transferProgress = 0.0;
      notifyListeners();
    });

    _imageSub = _bleService.imageReceived.listen((imageData) async {
      try {
        // Get the latest metadata from BLE service
        // For now, use default 160x120 since that's what firmware sends
        await _imageService.processAndSaveImage(imageData, 160, 120);
        _isCapturing = false;
        _transferProgress = 1.0;
        _errorMessage = null;
      } catch (e) {
        _errorMessage = 'Failed to process image: $e';
        _isCapturing = false;
      }
      notifyListeners();
    });

    // Load saved images in background
    Future.microtask(() async {
      try {
        await _imageService.loadSavedImages();
        notifyListeners();
      } catch (e) {
        print('Failed to load saved images: $e');
      }
    });
  }

  /// Start scanning for devices
  Future<void> startScan() async {
    if (_connectionStatus == ConnectionStatus.scanning) {
      return; // Already scanning
    }
    
    _connectionStatus = ConnectionStatus.scanning;
    _scanResults = [];
    _errorMessage = null;
    notifyListeners();

    try {
      await _bleService.startScan();
      // After timeout, update status
      Future.delayed(const Duration(seconds: 10), () {
        if (_connectionStatus == ConnectionStatus.scanning) {
          _connectionStatus = ConnectionStatus.disconnected;
          notifyListeners();
        }
      });
    } catch (e) {
      _errorMessage = 'Scan failed: $e';
      _connectionStatus = ConnectionStatus.disconnected;
      notifyListeners();
    }
  }

  /// Stop scanning
  Future<void> stopScan() async {
    try {
      await _bleService.stopScan();
    } catch (_) {}
    if (_connectionStatus == ConnectionStatus.scanning) {
      _connectionStatus = ConnectionStatus.disconnected;
      notifyListeners();
    }
  }

  /// Connect to a device
  Future<void> connect(BluetoothDevice device) async {
    _connectionStatus = ConnectionStatus.connecting;
    _errorMessage = null;
    notifyListeners();

    try {
      await _bleService.stopScan();
      await _bleService.connect(device);
      
      // Only request MTU on Android - iOS negotiates automatically
      if (Platform.isAndroid) {
        try {
          await _bleService.requestMtu(512);
        } catch (e) {
          print('MTU request failed (non-critical): $e');
        }
      }
      
      _connectionStatus = ConnectionStatus.connected;
      notifyListeners();
    } catch (e) {
      _errorMessage = 'Connection failed: $e';
      _connectionStatus = ConnectionStatus.disconnected;
      notifyListeners();
    }
  }

  /// Disconnect
  Future<void> disconnect() async {
    await _bleService.disconnect();
  }

  /// Trigger capture
  Future<void> capture() async {
    if (!isConnected) {
      _errorMessage = 'Not connected to device';
      notifyListeners();
      return;
    }

    try {
      _isCapturing = true;
      _errorMessage = null;
      notifyListeners();
      await _bleService.triggerCapture();
    } catch (e) {
      _errorMessage = 'Capture failed: $e';
      _isCapturing = false;
      notifyListeners();
    }
  }

  /// Delete an image
  Future<void> deleteImage(String id) async {
    await _imageService.deleteImage(id);
    notifyListeners();
  }

  /// Clear all images
  Future<void> clearAllImages() async {
    await _imageService.clearAll();
    notifyListeners();
  }

  void clearError() {
    _errorMessage = null;
    notifyListeners();
  }

  @override
  void dispose() {
    _connectionSub?.cancel();
    _scanSub?.cancel();
    _imageSub?.cancel();
    _metaSub?.cancel();
    _bleService.dispose();
    super.dispose();
  }
}
