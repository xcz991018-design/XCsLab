#ifndef FILE_CAMERA_SOURCE_HPP
#define FILE_CAMERA_SOURCE_HPP

#include "ICameraSource.hpp"
#include <string>
#include <vector>

class FileCameraSource : public ICameraSource {
public:
    explicit FileCameraSource(const std::string& imagePathOrDir);
    ~FileCameraSource() override;

    bool open() override;
    bool grabFrame(cv::Mat& frame) override;
    void close() override;
    bool isOpened() const override;
    std::string getSourceName() const override;

private:
    std::string m_path;
    bool m_isOpen{false};
    cv::Mat m_cachedFrame;
    std::vector<std::string> m_fileList;
    size_t m_currentIndex{0};
    bool m_isList{false};
};

#endif // FILE_CAMERA_SOURCE_HPP
