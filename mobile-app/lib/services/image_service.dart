import 'dart:io';
import 'dart:typed_data';
import 'package:image/image.dart' as img;
import 'package:path_provider/path_provider.dart';
import '../models/captured_image.dart';

/// Service for converting and storing images
class ImageService {
  static final ImageService _instance = ImageService._internal();
  factory ImageService() => _instance;
  ImageService._internal();

  final List<CapturedImage> _images = [];

  List<CapturedImage> get images => List.unmodifiable(_images);

  /// Convert RGB565 data to PNG and save
  Future<CapturedImage> processAndSaveImage(
    Uint8List rgb565Data,
    int width,
    int height,
  ) async {
    // Convert RGB565 to RGB888 image
    final image = img.Image(width: width, height: height);
    
    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        final index = (y * width + x) * 2;
        if (index + 1 >= rgb565Data.length) break;
        
        // RGB565 big-endian (matching firmware byte order fix)
        final high = rgb565Data[index];
        final low = rgb565Data[index + 1];
        final pixel = (high << 8) | low;
        
        // Extract RGB components
        final r5 = (pixel >> 11) & 0x1F;
        final g6 = (pixel >> 5) & 0x3F;
        final b5 = pixel & 0x1F;
        
        // Convert to 8-bit
        final r = (r5 << 3) | (r5 >> 2);
        final g = (g6 << 2) | (g6 >> 4);
        final b = (b5 << 3) | (b5 >> 2);
        
        image.setPixelRgb(x, y, r, g, b);
      }
    }

    // Encode as PNG
    final pngData = Uint8List.fromList(img.encodePng(image));

    // Save to file
    final directory = await getApplicationDocumentsDirectory();
    final timestamp = DateTime.now();
    final filename = 'veea_${timestamp.millisecondsSinceEpoch}.png';
    final filePath = '${directory.path}/$filename';
    
    final file = File(filePath);
    await file.writeAsBytes(pngData);

    // Create captured image object
    final capturedImage = CapturedImage(
      id: timestamp.millisecondsSinceEpoch.toString(),
      timestamp: timestamp,
      width: width,
      height: height,
      rawData: rgb565Data,
      filePath: filePath,
      pngData: pngData,
    );

    _images.insert(0, capturedImage);
    return capturedImage;
  }

  /// Load existing images from storage
  Future<void> loadSavedImages() async {
    final directory = await getApplicationDocumentsDirectory();
    final dir = Directory(directory.path);
    
    if (!await dir.exists()) return;

    final files = await dir.list().where((f) => f.path.endsWith('.png') && f.path.contains('veea_')).toList();
    
    for (var file in files) {
      if (file is File) {
        final filename = file.path.split('/').last;
        final timestampStr = filename.replaceAll('veea_', '').replaceAll('.png', '');
        final timestamp = int.tryParse(timestampStr);
        
        if (timestamp != null) {
          final pngData = await file.readAsBytes();
          final decodedImage = img.decodeImage(pngData);
          
          if (decodedImage != null) {
            final capturedImage = CapturedImage(
              id: timestampStr,
              timestamp: DateTime.fromMillisecondsSinceEpoch(timestamp),
              width: decodedImage.width,
              height: decodedImage.height,
              filePath: file.path,
              pngData: pngData,
            );
            _images.add(capturedImage);
          }
        }
      }
    }

    // Sort by timestamp (newest first)
    _images.sort((a, b) => b.timestamp.compareTo(a.timestamp));
  }

  /// Delete an image
  Future<void> deleteImage(String id) async {
    final index = _images.indexWhere((img) => img.id == id);
    if (index != -1) {
      final image = _images[index];
      if (image.filePath != null) {
        final file = File(image.filePath!);
        if (await file.exists()) {
          await file.delete();
        }
      }
      _images.removeAt(index);
    }
  }

  /// Clear all images
  Future<void> clearAll() async {
    for (var image in _images) {
      if (image.filePath != null) {
        final file = File(image.filePath!);
        if (await file.exists()) {
          await file.delete();
        }
      }
    }
    _images.clear();
  }
}
