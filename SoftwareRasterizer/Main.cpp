#include "Occluder.h"
#include "QuadDecomposition.h"
#include "Rasterizer.h"
#include "SurfaceAreaHeuristic.h"
#include "VectorMath.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#pragma warning(disable:4996)

#include <DirectXMath.h>
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <vector>

//#define USE_MOC
//#define USE_OBJ

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#include "MaskedOcclusionCulling.h"

using namespace DirectX;

#if 1
static constexpr uint32_t WINDOW_WIDTH = 1280;
static constexpr uint32_t WINDOW_HEIGHT = 720;
#else
static constexpr uint32_t WINDOW_WIDTH = 128;
static constexpr uint32_t WINDOW_HEIGHT = 128;
#endif

#ifndef USE_OBJ
#if 1
#define SCENE "Castle"
#define FOV 0.628f
XMVECTOR g_cameraPosition = XMVectorSet(27.0f, 2.0f, 47.0f, 0.0f);
XMVECTOR g_cameraDirection = XMVectorSet(0.142582759f, 0.0611068942f, -0.987894833f, 0.0f);
XMVECTOR g_upVector = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
#else
#define SCENE "Sponza"
#define FOV 1.04f
XMVECTOR g_cameraPosition = XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f);
XMVECTOR g_cameraDirection = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
XMVECTOR g_upVector = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
#endif
#else
#define FOV 1.04f
XMVECTOR g_cameraPosition = XMVectorSet(-3.0f, 0.0f, 0.0f, 0.0f);
XMVECTOR g_cameraDirection = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
XMVECTOR g_upVector = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
#endif

int nTotalTris = 0;

std::unique_ptr<Rasterizer> g_rasterizer;

MaskedOcclusionCulling *g_moc = nullptr;

HBITMAP g_hBitmap;
std::vector<std::unique_ptr<Occluder>> g_occluders;

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
  AllocConsole();
  freopen("CONOUT$", "w", stdout);
  
  // Flush denorms to zero to avoid performance issues with small values
  _mm_setcsr(_mm_getcsr() | 0x8040);

  std::vector<__m128> vertices;
  std::vector<uint32_t> indices;

#ifndef USE_OBJ
  {
    std::stringstream fileName;
    fileName << SCENE << "/IndexBuffer.bin";
    std::ifstream inFile(fileName.str(), std::ifstream::binary);

    inFile.seekg(0, std::ifstream::end);
    auto size = inFile.tellg();
    inFile.seekg(0);

    auto numIndices = size / sizeof indices[0];

    indices.resize(numIndices);
    inFile.read(reinterpret_cast<char*>(&indices[0]), numIndices * sizeof indices[0]);
  }

  {
    std::stringstream fileName;
    fileName << SCENE << "/VertexBuffer.bin";
    std::ifstream inFile(fileName.str(), std::ifstream::binary);

    inFile.seekg(0, std::ifstream::end);
    auto size = inFile.tellg();
    inFile.seekg(0);

    auto numVertices = size / sizeof vertices[0];

    vertices.resize(numVertices);
    inFile.read(reinterpret_cast<char*>(&vertices[0]), numVertices * sizeof vertices[0]);
  }
#else
  tinyobj::attrib_t attrib;
  std::vector<tinyobj::shape_t> shapes;
  std::vector<tinyobj::material_t> materials;

  std::string err;
  bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &err, "Models/cube.obj", nullptr, true);
  
  vertices.resize(attrib.vertices.size()/3);
  for( size_t ii = 0; ii < vertices.size(); ++ii )
  {
      vertices[ii] = { attrib.vertices[3 * ii], attrib.vertices[3 * ii + 1], attrib.vertices[3 * ii + 2], 1.0f};
  }

  for(size_t i = 0; i < shapes.size(); i++ )
  {
      size_t index_offset = 0;
      for( size_t f = 0; f < shapes[i].mesh.num_face_vertices.size(); f++ )
      {
          size_t fnum = shapes[i].mesh.num_face_vertices[f];

          for( size_t v = 0; v < fnum; v++ )
          {
              tinyobj::index_t idx = shapes[i].mesh.indices[index_offset + v];
              indices.push_back(idx.vertex_index);
          }
          //std::swap(indices[index_offset + 1], indices[index_offset + 2]);
          index_offset += fnum;
      }
  }
  
#endif
  nTotalTris = (int)indices.size() / 3;

  indices = QuadDecomposition::decompose(indices, vertices);

  g_rasterizer = std::make_unique<Rasterizer>(WINDOW_WIDTH, WINDOW_HEIGHT);

  g_moc = MaskedOcclusionCulling::Create(MaskedOcclusionCulling::SSE41);

  MaskedOcclusionCulling::Implementation implementation = g_moc->GetImplementation();
  switch (implementation) {
      case MaskedOcclusionCulling::SSE2: printf("Using SSE2 version\n"); break;
      case MaskedOcclusionCulling::SSE41: printf("Using SSE41 version\n"); break;
      case MaskedOcclusionCulling::AVX2: printf("Using AVX2 version\n"); break;
      case MaskedOcclusionCulling::AVX512: printf("Using AVX-512 version\n"); break;
  }

  g_moc->SetResolution(WINDOW_WIDTH, WINDOW_HEIGHT);
  
  
  // Pad to a multiple of 8 quads
  while (indices.size() % 32 != 0)
  {
    indices.push_back(indices[0]);
  }

  std::vector<Aabb> quadAabbs;
  for (auto quadIndex = 0; quadIndex < indices.size() / 4; ++quadIndex)
  {
    Aabb aabb;
    aabb.include(vertices[indices[4 * quadIndex + 0]]);
    aabb.include(vertices[indices[4 * quadIndex + 1]]);
    aabb.include(vertices[indices[4 * quadIndex + 2]]);
    aabb.include(vertices[indices[4 * quadIndex + 3]]);
    quadAabbs.push_back(aabb);
  }

  auto batchAssignment = SurfaceAreaHeuristic::generateBatches(quadAabbs, 512, 8);

  Aabb refAabb;
  for (auto v : vertices)
  {
    refAabb.include(v);
  }

  // Bake occluders
  for (const auto& batch : batchAssignment)
  {
    std::vector<__m128> batchVertices;
    for (auto quadIndex : batch)
    {
      batchVertices.push_back(vertices[indices[quadIndex * 4 + 0]]);
      batchVertices.push_back(vertices[indices[quadIndex * 4 + 1]]);
      batchVertices.push_back(vertices[indices[quadIndex * 4 + 2]]);
      batchVertices.push_back(vertices[indices[quadIndex * 4 + 3]]);
    }

    g_occluders.push_back(Occluder::bake(batchVertices, refAabb.m_min, refAabb.m_max));
  }

  WNDCLASSEXW wcex = {};

  wcex.cbSize = sizeof(WNDCLASSEX);

  wcex.style = CS_HREDRAW | CS_VREDRAW;
  wcex.lpfnWndProc = WndProc;
  wcex.hInstance = hInstance;
  wcex.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
  wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wcex.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wcex.lpszClassName = L"RasterizerWindow";

  ATOM windowClass = RegisterClassExW(&wcex);

  HWND hWnd = CreateWindowW(LPCWSTR(windowClass), L"Rasterizer", WS_SYSMENU,
    CW_USEDEFAULT, CW_USEDEFAULT, WINDOW_WIDTH, WINDOW_HEIGHT, nullptr, nullptr, hInstance, nullptr);

  HDC hdc = GetDC(hWnd);
  g_hBitmap = CreateCompatibleBitmap(hdc, WINDOW_WIDTH, WINDOW_HEIGHT);
  ReleaseDC(hWnd, hdc);

  ShowWindow(hWnd, SW_SHOW);
  UpdateWindow(hWnd);

  MSG msg;
  while (GetMessage(&msg, nullptr, 0, 0))
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }


  // Destroy occlusion culling object and free hierarchical z-buffer
  MaskedOcclusionCulling::Destroy(g_moc);
  return 0;
}

static void TonemapDepth(float *depth, unsigned char *image, int w, int h)
{
    // Find min/max w coordinate (discard cleared pixels)
    float minW = FLT_MAX, maxW = 0.0f;
    for (int i = 0; i < w*h; ++i)
    {
        if (depth[i] > 0.0f)
        {
            minW = std::min(minW, depth[i]);
            maxW = std::max(maxW, depth[i]);
        }
    }

    // Tonemap depth values
    for (int i = 0; i < w*h; ++i)
    {
        int intensity = 0;
        if (depth[i] > 0)
            intensity = (unsigned char)(223.0*(depth[i] - minW) / (maxW - minW) + 32.0);

        image[i * 4 + 0] = intensity;
        image[i * 4 + 1] = intensity;
        image[i * 4 + 2] = intensity;
        image[i * 4 + 3] = 255;
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  switch (message)
  {
  case WM_PAINT:
  {

    std::vector<char> rawData;
    rawData.resize(WINDOW_WIDTH * WINDOW_HEIGHT * 4);

   

    // Sort front to back
    std::sort(begin(g_occluders), end(g_occluders), [&](const auto& o1, const auto& o2) {
        __m128 dist1 = _mm_sub_ps(o1->m_center, g_cameraPosition);
        __m128 dist2 = _mm_sub_ps(o2->m_center, g_cameraPosition);

        return _mm_comile_ss(_mm_dp_ps(dist1, dist1, 0x7f), _mm_dp_ps(dist2, dist2, 0x7f));
    });

#ifndef USE_MOC
    XMMATRIX transMatrix = XMMatrixTranslation(1, 2, 3);
    XMMATRIX projMatrix = XMMatrixPerspectiveFovLH(FOV, float(WINDOW_WIDTH) / float(WINDOW_HEIGHT), 1.0f, 5000.0f);
    XMMATRIX viewMatrix = XMMatrixLookToLH(g_cameraPosition, g_cameraDirection, g_upVector);
    XMMATRIX viewProjection = (XMMatrixMultiply(viewMatrix, projMatrix));

    float mvp[16];

    memcpy(mvp, &viewProjection, 64);

    g_rasterizer->clear();
    g_rasterizer->setModelViewProjection(mvp);

    auto raster_start = std::chrono::high_resolution_clock::now();

    int nOccluderQuadsNeedClip = 0;
    int nOccluderQuadsNoClip = 0;
    int nDepthFailedQuads = 0;

    for (const auto& occluder : g_occluders)
    {
      bool needsClipping;
      if (g_rasterizer->queryVisibility(occluder->m_boundsMin, occluder->m_boundsMax, needsClipping))
      {
        if (needsClipping)
        {
            nOccluderQuadsNeedClip += g_rasterizer->rasterize<true>(*occluder);
        }
        else
        {
            nOccluderQuadsNoClip += g_rasterizer->rasterize<false>(*occluder);
        }
      }
      else
      {
          nDepthFailedQuads += occluder->m_packetCount / 4;
      }
    }

    auto raster_end = std::chrono::high_resolution_clock::now();

    float rasterTime = std::chrono::duration<float, std::milli>(raster_end - raster_start).count();
    static float avgRasterTime = rasterTime;

    float alpha = 0.0035f;
    avgRasterTime = rasterTime * alpha + avgRasterTime * (1.0f - alpha);

    int fps = int(1000.0f / avgRasterTime);

    std::wstringstream title;
    title << L"FPS: " << fps << std::setprecision(3) << L" r " << std::setprecision(3) << avgRasterTime << "ms"
        << " tris: " << nTotalTris << " q(df): " << nDepthFailedQuads
        << " q(c): " << nOccluderQuadsNeedClip << " q(nc) " << nOccluderQuadsNoClip;
    
    SetWindowText(hWnd, title.str().c_str());

    g_rasterizer->readBackDepth(&*rawData.begin());

#else
    XMMATRIX projMatrix = XMMatrixPerspectiveFovLH(FOV, float(WINDOW_WIDTH) / float(WINDOW_HEIGHT), 1.0f, 5000.0f);
    XMMATRIX viewMatrix = XMMatrixLookToLH(g_cameraPosition, g_cameraDirection, g_upVector);
    XMMATRIX viewProjection = (XMMatrixMultiply(viewMatrix, projMatrix));
    //XMMATRIX vpTransposed = XMMatrixTranspose(viewProjection);

    float mvp[16];

    memcpy(mvp, &viewProjection, 64);

    g_moc->ClearBuffer();
    
    float rasterTime = 0.f;

#if 0
    struct ClipspaceVertex { float x, y, z, w; };
    // A quad completely within the view frustum
    ClipspaceVertex quadVerts[] = { { -150, -150, 0, 200 },{ -10, -65, 0, 75 },{ 0, 0, 0, 20 },{ -40, 10, 0, 50 } };
    unsigned int quadIndices[] = { 0, 1, 2, 0, 2, 3 };

    g_moc->RenderTriangles((float*)&quadVerts, quadIndices, 2);
#else
    for (const auto& occluder : g_occluders)
    {
        uint32_t* quadIndices = new uint32_t[6 * occluder->m_vertexDataRaw.size()];
        for( int ii = 0, j = 0; ii < occluder->m_vertexDataRaw.size(); ii += 4, j += 6 )
        {
            quadIndices[j+ 0] = ii;
            quadIndices[j + 1] = ii + 1;
            quadIndices[j + 2] = ii + 2;
            quadIndices[j + 3] = ii;
            quadIndices[j + 4] = ii + 2;
            quadIndices[j + 5] = ii + 3;
        }
        auto raster_start = std::chrono::high_resolution_clock::now();
        g_moc->RenderTriangles((float*)occluder->m_vertexDataRaw.data(), quadIndices, (uint32_t)occluder->m_vertexDataRaw.size()/2, mvp, MaskedOcclusionCulling::BACKFACE_CW, MaskedOcclusionCulling::CLIP_PLANE_ALL, MaskedOcclusionCulling::VertexLayout(16, 4, 8));
        auto raster_end = std::chrono::high_resolution_clock::now();
        rasterTime += std::chrono::duration<float, std::milli>(raster_end - raster_start).count();

        delete[] quadIndices;
    }
#endif

    static float *perPixelZBuffer = nullptr;
    if(perPixelZBuffer == nullptr)
        perPixelZBuffer = new float[WINDOW_WIDTH*WINDOW_HEIGHT];

    g_moc->ComputePixelDepthBuffer(perPixelZBuffer, true);
    TonemapDepth(perPixelZBuffer, (uint8_t*)&*rawData.begin(), WINDOW_WIDTH, WINDOW_HEIGHT);

    static float avgRasterTime = rasterTime;

    float alpha = 0.0035f;
    avgRasterTime = rasterTime * alpha + avgRasterTime * (1.0f - alpha);

    int fps = int(1000.0f / avgRasterTime);

    std::wstringstream title;
    title << L"FPS: " << fps << std::setprecision(3) << L" r " << std::setprecision(3) << avgRasterTime << "ms";
    SetWindowText(hWnd, title.str().c_str());

#endif

    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);

    HDC hdcMem = CreateCompatibleDC(hdc);

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = WINDOW_WIDTH;
    info.bmiHeader.biHeight = WINDOW_HEIGHT;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    SetDIBits(hdcMem, g_hBitmap, 0, WINDOW_HEIGHT, &*rawData.begin(), &info, DIB_PAL_COLORS);

    BITMAP bm;
    HGDIOBJ hbmOld = SelectObject(hdcMem, g_hBitmap);

    GetObject(g_hBitmap, sizeof(bm), &bm);

    BitBlt(hdc, 0, 0, bm.bmWidth, bm.bmHeight, hdcMem, 0, 0, SRCCOPY);

    SelectObject(hdcMem, hbmOld);
    DeleteDC(hdcMem);

    EndPaint(hWnd, &ps);

    static auto lastPaint = std::chrono::high_resolution_clock::now();
    auto now = std::chrono::high_resolution_clock::now();

    XMVECTOR right = XMVector3Normalize(XMVector3Cross(g_cameraDirection, g_upVector));
    float translateSpeed = 0.01f * std::chrono::duration<float, std::milli>(now - lastPaint).count();
    float rotateSpeed = 0.002f * std::chrono::duration<float, std::milli>(now - lastPaint).count();

    lastPaint = now;

    if (GetAsyncKeyState(VK_SHIFT))
      translateSpeed *= 3.0f;

    if (GetAsyncKeyState(VK_CONTROL))
      translateSpeed *= 0.1f;

    if (GetAsyncKeyState('W'))
      g_cameraPosition = XMVectorAdd(g_cameraPosition, XMVectorMultiply(g_cameraDirection, XMVectorSet(translateSpeed, translateSpeed, translateSpeed, translateSpeed)));

    if (GetAsyncKeyState('S'))
      g_cameraPosition = XMVectorAdd(g_cameraPosition, XMVectorMultiply(g_cameraDirection, XMVectorSet(-translateSpeed, -translateSpeed, -translateSpeed, -translateSpeed)));

    if (GetAsyncKeyState('A'))
      g_cameraPosition = XMVectorAdd(g_cameraPosition, XMVectorMultiply(right, XMVectorSet(translateSpeed, translateSpeed, translateSpeed, translateSpeed)));

    if (GetAsyncKeyState('D'))
      g_cameraPosition = XMVectorAdd(g_cameraPosition, XMVectorMultiply(right, XMVectorSet(-translateSpeed, -translateSpeed, -translateSpeed, -translateSpeed)));

    if (GetAsyncKeyState(VK_UP))
      g_cameraDirection = XMVector3Rotate(g_cameraDirection, XMQuaternionRotationAxis(right, rotateSpeed));

    if (GetAsyncKeyState(VK_DOWN))
      g_cameraDirection = XMVector3Rotate(g_cameraDirection, XMQuaternionRotationAxis(right, -rotateSpeed));

    if (GetAsyncKeyState(VK_LEFT))
      g_cameraDirection = XMVector3Rotate(g_cameraDirection, XMQuaternionRotationAxis(g_upVector, -rotateSpeed));

    if (GetAsyncKeyState(VK_RIGHT))
      g_cameraDirection = XMVector3Rotate(g_cameraDirection, XMQuaternionRotationAxis(g_upVector, rotateSpeed));

    InvalidateRect(hWnd, nullptr, FALSE);
  }
  break;

  case WM_DESTROY:
    PostQuitMessage(0);
    break;

  default:
    return DefWindowProc(hWnd, message, wParam, lParam);
  }
  return 0;
}


