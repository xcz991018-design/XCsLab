#include "FileCameraSource.hpp"
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

FileCameraSource::FileCameraSource(const std::string& imagePathOrDir)
    : m_path(imagePathOrDir) {}

FileCameraSource::~FileCameraSource() {
    close();
}

bool FileCameraSource::open() {
    if (fs::is_regular_file(m_path)) {
        m_cachedFrame = cv::imread(m_path, cv::IMREAD_COLOR);
        if (m_cachedFrame.empty()) {
            std::cerr << "[FileCameraSource] Error: Failed to load image from: " << m_path << std::endl;
            m_isOpen = false;
            return false;
        }
        m_isList = false;
        m_isOpen = true;
        return true;
    } else if (fs::is_directory(m_path)) {
        m_fileList.clear();
        for (const auto& entry : fs::directory_iterator(m_path)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                if (ext == ".jpg" || ext == ".png" || ext == ".bmp" || ext == ".jpeg") {
                    m_fileList.push_back(entry.path().string());
                }
            }
        }
        if (m_fileList.empty()) {
            std::cerr << "[FileCameraSource] Error: No images found in directory: " << m_path << std::endl;
            m_isOpen = false;
            return false;
        }
        m_currentIndex = 0;
        m_isList = true;
        m_isOpen = true;
        return true;
    }

    std::cerr << "[FileCameraSource] Path does not exist: " << m_path << std::endl;
    m_isOpen = false;
    return false;
}

bool FileCameraSource::grabFrame(cv::Mat& frame) {
    if (!m_isOpen) return false;

    if (!m_isList) {
        if (m_cachedFrame.empty()) return false;
        frame = m_cachedFrame.clone();
        return true;
    } else {
        if (m_currentIndex >= m_fileList.size()) return false;
        std::string currentPath = m_fileList[m_currentIndex++];
        frame = cv::imread(currentPath, cv::IMREAD_COLOR);
        return !frame.empty();
    }
}

void FileCameraSource::close() {
    m_isOpen = false;
    m_cachedFrame.release();
    m_fileList.clear();
}

bool FileCameraSource::isOpened() const {
    return m_isOpen;
}

std::string FileCameraSource::getSourceName() const {
    return "FileSource(" + m_path + ")";
}
