/*
 * DisplayWidget -- see DisplayWidget.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DisplayWidget.h"

#include "DisplayGeometry.h"

#include <QEnterEvent>
#include <QEvent>
#include <QMouseEvent>
#include <QFile>
#include <QMatrix4x4>
#include <QVector4D>
#include <QWindow>

static QShader loadShader(const char *path)
{
    QFile f(QString::fromLatin1(path));
    return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
}

DisplayWidget::DisplayWidget(a7800session *session, QWidget *parent)
    : QRhiWidget(parent), m_session(session), m_feed(new FrameFeed(session, this)),
      m_scratch(A7800SESSION_FB_WIDTH, A7800SESSION_FB_MAX_HEIGHT, QImage::Format_RGB32)
{
    m_scratch.fill(0);
    setMinimumSize(320, 224);
    /* motion without a button held aims a light gun */
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    connect(m_feed, &FrameFeed::frameReady, this, &DisplayWidget::onFrame);
}

DisplayWidget::~DisplayWidget()
{
    m_feed->setActive(false);
}

void DisplayWidget::setAspect(int aspect) { m_aspect = aspect; update(); }
void DisplayWidget::setSmooth(bool smooth) { m_smooth = smooth; update(); }

/* ---- idle ------------------------------------------------------------------
 * A hidden or minimised window has nobody to draw for: the feed unregisters
 * from the session, so the GUI thread gets no wakeups at all until it is
 * shown again. The machine itself keeps running (netplay must not stall). */

void DisplayWidget::updateActive()
{
    QWidget *w = window();
    m_feed->setActive(isVisible() && w && !w->isMinimized());
}

void DisplayWidget::showEvent(QShowEvent *e)
{
    QRhiWidget::showEvent(e);
    if (window())
        window()->installEventFilter(this);
    updateActive();
}

void DisplayWidget::hideEvent(QHideEvent *e)
{
    QRhiWidget::hideEvent(e);
    if (window())
        window()->removeEventFilter(this);
    updateActive();
}

bool DisplayWidget::eventFilter(QObject *watched, QEvent *e)
{
    if (e->type() == QEvent::WindowStateChange || e->type() == QEvent::Show ||
        e->type() == QEvent::Hide)
        updateActive();
    return QRhiWidget::eventFilter(watched, e);
}

bool DisplayWidget::event(QEvent *e)
{
    /* The compositor can mark a window occluded without minimising it */
    if (e->type() == QEvent::ScreenChangeInternal || e->type() == QEvent::WindowActivate)
        updateActive();
    return QRhiWidget::event(e);
}

/* ---- frames ---------------------------------------------------------------- */

void DisplayWidget::onFrame()
{
    int h = 0;
    syncGun();
    /* straight into the scratch image: one copy, and no alpha pass -- the
     * shader ignores the X byte */
    if (!a7800session_copy_frame(m_session, reinterpret_cast<uint32_t *>(m_scratch.bits()),
                               &h, &m_serial))
        return;
    if (h <= 0 || h > A7800SESSION_FB_MAX_HEIGHT)
        return;
    m_lines = h;
    m_dirty = true;
    update();
}

/* ---- the light gun ----------------------------------------------------------
 * A gun comes and goes with the controller type (and with AUTO, with the
 * game). There is no polling timer: the cursor follows on each new frame and
 * whenever the mouse is about, which is when it matters. */

void DisplayWidget::syncGun()
{
    const bool gun = a7800session_lightgun_active(m_session) != 0;
    if (gun == m_gun)
        return;
    m_gun = gun;
    if (gun) setCursor(Qt::CrossCursor);
    else {
        unsetCursor();
        a7800session_pointer(m_session, 0, 0, 0, 0);
    }
}

/* Widget coordinates to frame pixels; bit 0 the primary button (fires every
 * light gun), bit 1 the secondary (fires off-screen, a reload in some
 * games). */
void DisplayWidget::sendPointer(const QPointF &pos, Qt::MouseButtons buttons)
{
    syncGun();
    if (!m_gun || m_lines <= 0) return;
    const QRectF r = displayRect(QSizeF(size()), m_lines, m_aspect == 0);
    if (r.isEmpty()) return;
    const int x = (int)((pos.x() - r.x()) * A7800SESSION_FB_WIDTH / r.width());
    const int y = (int)((pos.y() - r.y()) * m_lines / r.height());
    const int inside = r.contains(pos) ? 1 : 0;
    unsigned b = 0;
    if (buttons & Qt::LeftButton) b |= 1u;
    if (buttons & Qt::RightButton) b |= 2u;
    a7800session_pointer(m_session, qBound(0, x, A7800SESSION_FB_WIDTH - 1),
                         qBound(0, y, m_lines - 1), inside, b);
}

void DisplayWidget::mouseMoveEvent(QMouseEvent *e)
{
    sendPointer(e->position(), e->buttons());
    QRhiWidget::mouseMoveEvent(e);
}

void DisplayWidget::mousePressEvent(QMouseEvent *e)
{
    sendPointer(e->position(), e->buttons());
    QRhiWidget::mousePressEvent(e);
}

void DisplayWidget::mouseReleaseEvent(QMouseEvent *e)
{
    sendPointer(e->position(), e->buttons());
    QRhiWidget::mouseReleaseEvent(e);
}

void DisplayWidget::enterEvent(QEnterEvent *e)
{
    syncGun();
    QRhiWidget::enterEvent(e);
}

void DisplayWidget::leaveEvent(QEvent *e)
{
    if (m_gun) a7800session_pointer(m_session, 0, 0, 0, 0);
    QRhiWidget::leaveEvent(e);
}

/* ---- GPU ------------------------------------------------------------------- */

void DisplayWidget::releaseResources()
{
    m_pipeline.reset();
    m_srb.reset();
    m_linear.reset();
    m_nearest.reset();
    m_tex.reset();
    m_ubuf.reset();
    m_texLines = 0;
}

void DisplayWidget::rebuildTexture(int lines)
{
    m_tex.reset(m_rhi->newTexture(QRhiTexture::BGRA8, QSize(A7800SESSION_FB_WIDTH, lines)));
    m_tex->create();
    m_texLines = lines;
    m_srb.reset();   /* it names the old texture */
    /* A new texture is empty. A still picture publishes nothing more, so
     * re-upload what the scratch image already holds. */
    if (m_lines > 0)
        m_dirty = true;
}

void DisplayWidget::initialize(QRhiCommandBuffer *)
{
    if (m_rhi != rhi()) {
        releaseResources();
        m_rhi = rhi();
    }
    if (m_pipeline)
        return;

    m_ubuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 16));
    m_ubuf->create();
    m_nearest.reset(m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                      QRhiSampler::None, QRhiSampler::ClampToEdge,
                                      QRhiSampler::ClampToEdge));
    m_nearest->create();
    m_linear.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                     QRhiSampler::None, QRhiSampler::ClampToEdge,
                                     QRhiSampler::ClampToEdge));
    m_linear->create();
    if (!m_tex)
        rebuildTexture(m_lines > 0 ? m_lines : 224);

    m_smoothApplied = m_smooth;
    m_srb.reset(m_rhi->newShaderResourceBindings());
    m_srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, m_ubuf.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                  m_tex.get(), m_smooth ? m_linear.get() : m_nearest.get()),
    });
    m_srb->create();

    m_pipeline.reset(m_rhi->newGraphicsPipeline());
    m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_pipeline->setShaderStages({
        { QRhiShaderStage::Vertex, loadShader(":/shaders/display.vert.qsb") },
        { QRhiShaderStage::Fragment, loadShader(":/shaders/display.frag.qsb") },
    });
    m_pipeline->setVertexInputLayout({});
    m_pipeline->setShaderResourceBindings(m_srb.get());
    m_pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!m_pipeline->create()) {
        m_pipeline.reset();
        emit renderFailed();
    }
}

void DisplayWidget::render(QRhiCommandBuffer *cb)
{
    if (!m_pipeline)
        return;
    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();

    /* A console switch between NTSC and PAL changes the frame's height: the
     * texture is exactly the frame's size so the sampler never blends in
     * a stale line, and the bindings are rebuilt around it. */
    bool rebind = false;
    if (m_lines > 0 && m_lines != m_texLines) {
        rebuildTexture(m_lines);
        rebind = true;
    }
    if (m_smooth != m_smoothApplied) {
        m_smoothApplied = m_smooth;
        rebind = true;
    }
    if (rebind || !m_srb) {
        m_srb.reset(m_rhi->newShaderResourceBindings());
        m_srb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, m_ubuf.get()),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                      m_tex.get(), m_smooth ? m_linear.get() : m_nearest.get()),
        });
        m_srb->create();
    }

    if (m_dirty && m_lines > 0) {
        QRhiTextureSubresourceUploadDescription sub;
        const qsizetype bytes = (qsizetype)A7800SESSION_FB_WIDTH * m_lines * 4;
        sub.setData(QByteArray::fromRawData(reinterpret_cast<const char *>(m_scratch.constBits()), bytes));
        sub.setDataStride(A7800SESSION_FB_WIDTH * 4);
        sub.setSourceSize(QSize(A7800SESSION_FB_WIDTH, m_lines));
        u->uploadTexture(m_tex.get(), QRhiTextureUploadDescription({ QRhiTextureUploadEntry(0, 0, sub) }));
        m_dirty = false;
    }

    const QSize px = renderTarget()->pixelSize();
    const int lines = m_lines > 0 ? m_lines : 224;
    const QRectF r = displayRect(QSizeF(px), lines, m_aspect == 0);

    /* pixel rect -> clip space (y up), then through the backend's correction
     * so Vulkan's flipped y comes out the right way up */
    auto clip = [&](double x, double y) {
        return m_rhi->clipSpaceCorrMatrix() *
               QVector4D(float(x / px.width() * 2.0 - 1.0), float(1.0 - y / px.height() * 2.0), 0.f, 1.f);
    };
    const QVector4D tl = clip(r.left(), r.top());
    const QVector4D br = clip(r.right(), r.bottom());
    const float rect[4] = { tl.x(), tl.y(), br.x(), br.y() };
    u->updateDynamicBuffer(m_ubuf.get(), 0, sizeof rect, rect);

    cb->beginPass(renderTarget(), Qt::black, { 1.0f, 0 }, u);
    cb->setGraphicsPipeline(m_pipeline.get());
    cb->setViewport(QRhiViewport(0, 0, px.width(), px.height()));
    cb->setShaderResources(m_srb.get());
    cb->draw(4);
    cb->endPass();
}
