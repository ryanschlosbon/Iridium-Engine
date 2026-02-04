#include <fstream>
#include <vector>

static std::vector<char> readFile(const std::string& filename) {
	// Open file at end and in binary mode
	std::ifstream file(filename, std::ios::ate | std::ios::binary);

	if (!file.is_open()) {
		throw std::runtime_error("failed to open file: " + filename);
	}

	// Since we are at the end, tellg() gives us the file size
	size_t fileSize = (size_t)file.tellg();
	std::vector<char> buffer(fileSize);

	file.seekg(0); // Go back to the beginning
	file.read(buffer.data(), fileSize); // Read all bytes
	file.close();

	return buffer;
}