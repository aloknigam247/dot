#pragma once

#include "ImageModel.h"

#include <memory>
#include <QBuffer>
#include <QColor>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QImageReader>
#include <QImageWriter>

namespace dot::test {

using CreateResult = std::expected<quint32, QString>;

inline QByteArray encodePng(const QImage &image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if(!buffer.open(QIODevice::WriteOnly)) {
        qWarning() << "Could not open the PNG fixture buffer";
        return {};
    }
    QImageWriter writer(&buffer, "png");
    if(!writer.write(image)) {
        qWarning() << writer.errorString() << "readers:" << QImageReader::supportedImageFormats()
                   << "writers:" << QImageWriter::supportedImageFormats();
        return {};
    }
    return bytes;
}

inline QByteArray makePng(const QSize &size = QSize(4, 4),
                          QImage::Format format = QImage::Format_ARGB32,
                          const QColor &color = QColor(17, 34, 51)) {
    QImage image(size, format);
    image.fill(color);
    return encodePng(image);
}

inline CreateResult createSync(ImageModel &model, const QByteArray &png,
                               const ImageOptions &options = {}) {
    QElapsedTimer timer;
    const auto result = std::make_shared<std::optional<CreateResult>>();
    timer.start();
    model.create(png, options, [result](CreateResult value) { *result = std::move(value); });
    while(!*result && timer.elapsed() < 15000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    if(!*result) return std::unexpected(QStringLiteral("Timed out waiting for image decoding"));
    return std::move(**result);
}

} // namespace dot::test
