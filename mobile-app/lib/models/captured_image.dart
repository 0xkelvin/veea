import 'dart:typed_data';

/// Represents a captured image from the Veea device
class CapturedImage {
  final String id;
  final DateTime timestamp;
  final int width;
  final int height;
  final Uint8List? rawData; // RGB565 raw data
  final String? filePath; // Path to saved PNG file
  final Uint8List? pngData; // Converted PNG data

  CapturedImage({
    required this.id,
    required this.timestamp,
    required this.width,
    required this.height,
    this.rawData,
    this.filePath,
    this.pngData,
  });

  CapturedImage copyWith({
    String? id,
    DateTime? timestamp,
    int? width,
    int? height,
    Uint8List? rawData,
    String? filePath,
    Uint8List? pngData,
  }) {
    return CapturedImage(
      id: id ?? this.id,
      timestamp: timestamp ?? this.timestamp,
      width: width ?? this.width,
      height: height ?? this.height,
      rawData: rawData ?? this.rawData,
      filePath: filePath ?? this.filePath,
      pngData: pngData ?? this.pngData,
    );
  }
}
