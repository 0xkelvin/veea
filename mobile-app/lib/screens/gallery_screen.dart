import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../providers/camera_provider.dart';
import '../models/captured_image.dart';
import 'image_viewer_screen.dart';

class GalleryScreen extends StatelessWidget {
  const GalleryScreen({super.key});

  @override
  Widget build(BuildContext context) {
    return Consumer<CameraProvider>(
      builder: (context, provider, child) {
        final images = provider.images;

        return Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
              child: Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  Text(
                    'Gallery (${images.length})',
                    style: Theme.of(context).textTheme.titleMedium,
                  ),
                  if (images.isNotEmpty)
                    TextButton.icon(
                      onPressed: () => _showClearDialog(context, provider),
                      icon: const Icon(Icons.delete_outline, size: 18),
                      label: const Text('Clear All'),
                    ),
                ],
              ),
            ),
            Expanded(
              child: GridView.builder(
                padding: const EdgeInsets.all(8),
                gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
                  crossAxisCount: 3,
                  crossAxisSpacing: 8,
                  mainAxisSpacing: 8,
                ),
                itemCount: images.length,
                itemBuilder: (context, index) {
                  return _buildImageTile(context, images[index], provider);
                },
              ),
            ),
          ],
        );
      },
    );
  }

  Widget _buildImageTile(
    BuildContext context,
    CapturedImage image,
    CameraProvider provider,
  ) {
    return GestureDetector(
      onTap: () => Navigator.push(
        context,
        MaterialPageRoute(
          builder: (_) => ImageViewerScreen(image: image),
        ),
      ),
      onLongPress: () => _showDeleteDialog(context, image, provider),
      child: Hero(
        tag: 'image_${image.id}',
        child: Container(
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(8),
            color: Colors.grey.shade200,
          ),
          clipBehavior: Clip.antiAlias,
          child: image.pngData != null
              ? Image.memory(
                  image.pngData!,
                  fit: BoxFit.cover,
                )
              : const Center(
                  child: Icon(Icons.image, color: Colors.grey),
                ),
        ),
      ),
    );
  }

  void _showDeleteDialog(
    BuildContext context,
    CapturedImage image,
    CameraProvider provider,
  ) {
    showDialog(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Delete Image'),
        content: const Text('Are you sure you want to delete this image?'),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context),
            child: const Text('Cancel'),
          ),
          TextButton(
            onPressed: () {
              provider.deleteImage(image.id);
              Navigator.pop(context);
            },
            style: TextButton.styleFrom(foregroundColor: Colors.red),
            child: const Text('Delete'),
          ),
        ],
      ),
    );
  }

  void _showClearDialog(BuildContext context, CameraProvider provider) {
    showDialog(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Clear All Images'),
        content: const Text('Are you sure you want to delete all images? This cannot be undone.'),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context),
            child: const Text('Cancel'),
          ),
          TextButton(
            onPressed: () {
              provider.clearAllImages();
              Navigator.pop(context);
            },
            style: TextButton.styleFrom(foregroundColor: Colors.red),
            child: const Text('Clear All'),
          ),
        ],
      ),
    );
  }
}
