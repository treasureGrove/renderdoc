/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "AgentAssistantPanel.h"
#include <QClipboard>
#include <QComboBox>
#include <QLineEdit>
#include <QStringList>
#include <initializer_list>
#include <cstdint>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QString>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTextStream>
#include <QUrlQuery>
#include "Code/Interface/PersistantConfig.h"
#include "Code/QRDUtils.h"
#include "ui_AgentAssistantPanel.h"

enum class AgentLLMBackend : int
{
  OpenAI = 0,
  Anthropic = 1,
  GoogleGemini = 2,
  AzureOpenAI = 3,
  OpenAICompatible = 4,
  OpenRouter = 5,
  GLM_Zhipu = 6,
  GitHubModels = 7,
};

// Bitmask: which data "skills" to attach to the LLM context for the current question (Pipeline Agent).
enum AgentDataSkill : uint32_t
{
  AgentSkill_CaptureMeta = 1u << 0,
  AgentSkill_Action = 1u << 1,
  AgentSkill_PipelineSummary = 1u << 2,
  AgentSkill_ViewGeom = 1u << 3,
  AgentSkill_Shaders = 1u << 4,
  AgentSkill_OMTargets = 1u << 5,
  AgentSkill_BlendStencil = 1u << 6,
  AgentSkill_Descriptors = 1u << 7,
  AgentSkill_DescriptorAccess = 1u << 8,
  AgentSkill_ShaderMessages = 1u << 9,
  AgentSkill_ResourceBrief = 1u << 10,
  AgentSkill_AssetCatalog = 1u << 11,
  AgentSkill_All = 0xFFFFFFFFu
};

static bool questionContainsAny(const QString &haystack, const std::initializer_list<const char *> &subs)
{
  const QString h = haystack.toLower();
  for(const char *s : subs)
  {
    if(!s || !s[0])
      continue;
    if(h.contains(QString::fromUtf8(s), Qt::CaseInsensitive))
      return true;
  }
  return false;
}

static uint32_t selectAgentDataSkills(const QString &question)
{
  const QString q = question.trimmed();
  if(q.size() < 2)
    return AgentSkill_All;

  uint32_t m = 0;

  if(questionContainsAny(q, {"blend", "mrt", "alpha", "premulti", "混合", "混色"}))
    m |= AgentSkill_BlendStencil | AgentSkill_OMTargets;

  if(questionContainsAny(q, {"depth", "stencil", "z-test", "ztest", "深度", "模板"}))
    m |= AgentSkill_BlendStencil | AgentSkill_OMTargets;

  if(questionContainsAny(q, {"texture", "srv", "uav", "sample", "sampler", "bind", "descriptor",
                             "binding", "resource", "贴图", "纹理", "采样", "描述符", "绑定", "图像"}))
    m |= AgentSkill_Descriptors | AgentSkill_ResourceBrief | AgentSkill_OMTargets | AgentSkill_Shaders |
         AgentSkill_AssetCatalog;

  if(questionContainsAny(q, {"shader", "disasm", "disassemble", "spirv", "hlsl", "dxil", "assembly",
                             "着色器", "反编译", "汇编"}))
    m |= AgentSkill_Shaders | AgentSkill_Descriptors;

  if(questionContainsAny(q, {"model", "mesh", "网格", "模型"}))
    m |= AgentSkill_AssetCatalog | AgentSkill_ViewGeom | AgentSkill_PipelineSummary;

  if(questionContainsAny(q, {"catalog", "清单", "列表", "所有贴图", "所有纹理", "所有资源", "资源列表",
                             "capture-wide", "全局资源"}))
    m |= AgentSkill_AssetCatalog;

  if(questionContainsAny(q, {"vertex", "index", "indices", "vb", "ib", "layout", "input", "顶点", "索引",
                             "几何", "图元", "topology"}))
    m |= AgentSkill_ViewGeom | AgentSkill_PipelineSummary;

  if(questionContainsAny(q, {"marker", "region", "pass", "eid", "event", "action", "标记", "区域", "事件",
                             "范围"}))
    m |= AgentSkill_Action | AgentSkill_DescriptorAccess;

  if(questionContainsAny(q, {"dispatch", "compute", "threadgroup", "workgroup", "计算", "调度"}))
    m |= AgentSkill_Action | AgentSkill_PipelineSummary | AgentSkill_Descriptors | AgentSkill_Shaders;

  if(questionContainsAny(q, {"printf", "print", "message", "调试输出"}))
    m |= AgentSkill_ShaderMessages;

  if(questionContainsAny(q, {"viewport", "scissor", "raster", "视口", "裁剪", "光栅"}))
    m |= AgentSkill_ViewGeom | AgentSkill_PipelineSummary;

  if(questionContainsAny(q, {"output", "rtv", "render target", "fbo", "framebuffer", "颜色输出", "渲染目标",
                             "attachment"}))
    m |= AgentSkill_OMTargets | AgentSkill_BlendStencil;

  if(m == 0)
    return AgentSkill_All;

  m |= AgentSkill_CaptureMeta | AgentSkill_Action;
  return m;
}

static QString agentDataSkillMaskSummary(uint32_t mask)
{
  if(mask == AgentSkill_All)
    return lit("(all sections)");

  QStringList parts;
  if(mask & AgentSkill_CaptureMeta)
    parts << lit("capture");
  if(mask & AgentSkill_Action)
    parts << lit("action");
  if(mask & AgentSkill_PipelineSummary)
    parts << lit("pipeline");
  if(mask & AgentSkill_ViewGeom)
    parts << lit("view-geom");
  if(mask & AgentSkill_Shaders)
    parts << lit("shaders");
  if(mask & AgentSkill_OMTargets)
    parts << lit("om-targets");
  if(mask & AgentSkill_BlendStencil)
    parts << lit("blend-stencil");
  if(mask & AgentSkill_Descriptors)
    parts << lit("descriptors");
  if(mask & AgentSkill_DescriptorAccess)
    parts << lit("descriptor-access");
  if(mask & AgentSkill_ShaderMessages)
    parts << lit("shader-msgs");
  if(mask & AgentSkill_ResourceBrief)
    parts << lit("resource-brief");
  if(mask & AgentSkill_AssetCatalog)
    parts << lit("asset-catalog");
  return parts.join(lit(", "));
}

static QString rdcToQString(const rdcstr &s);

static uint32_t agentMinU32(uint32_t a, uint32_t b)
{
  return a < b ? a : b;
}

static void appendCaptureAssetCatalog(QTextStream &ts, ICaptureContext &ctx)
{
  const rdcarray<TextureDescription> &texs = ctx.GetTextures();
  const rdcarray<BufferDescription> &bufs = ctx.GetBuffers();
  const rdcarray<ResourceDescription> &regs = ctx.GetResources();

  const uint32_t kMaxTex = 128;
  const uint32_t kMaxBuf = 128;
  const uint32_t kMaxReg = 256;

  ts << lit(
      "Capture-wide asset catalog (metadata from the opened .rdc; not texel/pixel or buffer bytes).\n");
  ts << lit(
      "For mesh/vertex data of the current draw, use the view-geom section (IB/VB, vertex input layout, "
      "topology).\n\n");

  ts << lit("Textures in capture: total=") << (uint32_t)texs.size() << lit(" (showing first ")
     << agentMinU32(kMaxTex, (uint32_t)texs.size()) << lit(")\n");
  for(uint32_t i = 0; i < kMaxTex && i < (uint32_t)texs.size(); i++)
  {
    const TextureDescription &t = texs[i];
    ts << lit("  [") << i << lit("] ") << rdcToQString(ToStr(t.resourceId)) << lit(" \"")
       << rdcToQString(ctx.GetResourceName(t.resourceId)) << lit("\" ") << t.width << lit("x") << t.height
       << lit("x") << t.depth << lit(" dim=") << t.dimension << lit(" type=")
       << rdcToQString(ToStr(t.type)) << lit(" fmt=") << rdcToQString(ToStr(t.format)) << lit(" mips=")
       << t.mips << lit(" arraysize=") << t.arraysize << lit(" cubemap=")
       << (t.cubemap ? lit("yes") : lit("no")) << lit(" msSamp=") << t.msSamp << lit(" approxBytes=")
       << (qulonglong)t.byteSize << lit(" category=") << rdcToQString(ToStr(t.creationFlags)) << lit("\n");
  }
  if((uint32_t)texs.size() > kMaxTex)
    ts << lit("  ... ") << ((uint32_t)texs.size() - kMaxTex) << lit(" more textures omitted\n");
  ts << lit("\n");

  ts << lit("Buffers in capture: total=") << (uint32_t)bufs.size() << lit(" (showing first ")
     << agentMinU32(kMaxBuf, (uint32_t)bufs.size()) << lit(")\n");
  for(uint32_t i = 0; i < kMaxBuf && i < (uint32_t)bufs.size(); i++)
  {
    const BufferDescription &b = bufs[i];
    ts << lit("  [") << i << lit("] ") << rdcToQString(ToStr(b.resourceId)) << lit(" \"")
       << rdcToQString(ctx.GetResourceName(b.resourceId)) << lit("\" bytes=") << (qulonglong)b.length
       << lit(" category=") << rdcToQString(ToStr(b.creationFlags)) << lit(" gpuAddr=0x")
       << QString::number((qulonglong)b.gpuAddress, 16) << lit("\n");
  }
  if((uint32_t)bufs.size() > kMaxBuf)
    ts << lit("  ... ") << ((uint32_t)bufs.size() - kMaxBuf) << lit(" more buffers omitted\n");
  ts << lit("\n");

  ts << lit("Resource registry (all ResourceId entries): total=") << (uint32_t)regs.size()
     << lit(" (showing first ") << agentMinU32(kMaxReg, (uint32_t)regs.size()) << lit(")\n");
  for(uint32_t i = 0; i < kMaxReg && i < (uint32_t)regs.size(); i++)
  {
    const ResourceDescription &r = regs[i];
    ts << lit("  [") << i << lit("] ") << rdcToQString(ToStr(r.resourceId)) << lit(" type=")
       << rdcToQString(ToStr(r.type)) << lit(" name=\"") << rdcToQString(ctx.GetResourceName(r.resourceId))
       << lit("\" autoGen=") << (ctx.IsAutogeneratedName(r.resourceId) ? lit("yes") : lit("no"))
       << lit("\n");
  }
  if((uint32_t)regs.size() > kMaxReg)
    ts << lit("  ... ") << ((uint32_t)regs.size() - kMaxReg) << lit(" more registry entries omitted\n");
  ts << lit("\n");
}

static QString rdcToQString(const rdcstr &s)
{
  if(s.empty())
    return QString();
  return QString::fromUtf8(s.data(), (int)s.size());
}

static rdcstr qstrToRdc(const QString &s)
{
  QByteArray b = s.toUtf8();
  return rdcstr(b.data(), b.size());
}

static void appendShaderReflectionBindingSummary(rdcstr &out, const ShaderReflection *refl)
{
  if(!refl)
    return;

  out += "Shader reflection layout: ";
  out += ToStr(refl->encoding);
  out += " entry=";
  out += refl->entryPoint;
  out += " dispatchThreads ";
  out += ToStr(refl->dispatchThreadsDimension[0]);
  out += " ";
  out += ToStr(refl->dispatchThreadsDimension[1]);
  out += " ";
  out += ToStr(refl->dispatchThreadsDimension[2]);
  out += "\n";

  const size_t kMaxList = 32;
  out += "  Constant blocks (";
  out += ToStr((uint32_t)refl->constantBlocks.size());
  out += "):\n";
  for(size_t i = 0; i < refl->constantBlocks.size() && i < kMaxList; i++)
  {
    const ConstantBlock &cb = refl->constantBlocks[i];
    out += "    [";
    out += ToStr((uint32_t)i);
    out += "] ";
    out += cb.name;
    out += " set/space=";
    out += ToStr(cb.fixedBindSetOrSpace);
    out += " bind=";
    out += ToStr(cb.fixedBindNumber);
    out += " bytes=";
    out += ToStr(cb.byteSize);
    out += "\n";
  }
  if(refl->constantBlocks.size() > kMaxList)
    out += "    ... more constant blocks omitted\n";

  out += "  Read-only resources (";
  out += ToStr((uint32_t)refl->readOnlyResources.size());
  out += "):\n";
  for(size_t i = 0; i < refl->readOnlyResources.size() && i < kMaxList; i++)
  {
    const ShaderResource &sr = refl->readOnlyResources[i];
    out += "    [";
    out += ToStr((uint32_t)i);
    out += "] ";
    out += sr.name;
    out += " ";
    out += ToStr(sr.descriptorType);
    out += " ";
    out += ToStr(sr.textureType);
    out += " bind=";
    out += ToStr(sr.fixedBindNumber);
    out += "\n";
  }
  if(refl->readOnlyResources.size() > kMaxList)
    out += "    ... more read-only resources omitted\n";

  out += "  Read/write resources (";
  out += ToStr((uint32_t)refl->readWriteResources.size());
  out += "):\n";
  for(size_t i = 0; i < refl->readWriteResources.size() && i < kMaxList; i++)
  {
    const ShaderResource &sr = refl->readWriteResources[i];
    out += "    [";
    out += ToStr((uint32_t)i);
    out += "] ";
    out += sr.name;
    out += " ";
    out += ToStr(sr.descriptorType);
    out += " ";
    out += ToStr(sr.textureType);
    out += " bind=";
    out += ToStr(sr.fixedBindNumber);
    out += "\n";
  }
  if(refl->readWriteResources.size() > kMaxList)
    out += "    ... more read/write resources omitted\n";

  out += "  Samplers (";
  out += ToStr((uint32_t)refl->samplers.size());
  out += ")\n";
}

static void appendSamplerDetailLine(QTextStream &ts, ICaptureContext &ctx, const SamplerDescriptor &s)
{
  if(s.type != DescriptorType::Sampler && s.type != DescriptorType::ImageSampler)
    return;
  ts << lit("           Sampler: obj=") << rdcToQString(ToStr(s.object)) << lit(" \"")
     << rdcToQString(ctx.GetResourceName(s.object)) << lit("\" filter=")
     << rdcToQString(ToStr(s.filter)) << lit(" addrUVW=") << rdcToQString(ToStr(s.addressU))
     << lit("/") << rdcToQString(ToStr(s.addressV)) << lit("/") << rdcToQString(ToStr(s.addressW))
     << lit(" compare=") << rdcToQString(ToStr(s.compareFunction)) << lit(" lod=[")
     << s.minLOD << lit(",") << s.maxLOD << lit("] bias=") << s.mipBias << lit(" maxAniso=")
     << s.maxAnisotropy << lit(" unormCoords=") << (s.unnormalized ? lit("yes") : lit("no"))
     << lit(" seamlessCube=") << (s.seamlessCubemaps ? lit("yes") : lit("no")) << lit("\n");
}

static void appendDescriptorDetailLine(QTextStream &ts, ICaptureContext &ctx, const Descriptor &d)
{
  if(d.resource == ResourceId() && d.type == DescriptorType::Unknown)
  {
    ts << lit("           (empty descriptor)\n");
    return;
  }

  ts << lit("           Descriptor: type=") << rdcToQString(ToStr(d.type)) << lit(" viewFmt=")
     << rdcToQString(ToStr(d.format)) << lit(" texType=") << rdcToQString(ToStr(d.textureType))
     << lit(" view=") << rdcToQString(ToStr(d.view)) << lit(" mips[") << (uint32_t)d.firstMip
     << lit("+") << (uint32_t)d.numMips << lit("] slices[") << d.firstSlice << lit("+")
     << d.numSlices << lit("] swizzle=") << rdcToQString(ToStr(d.swizzle)) << lit(" minLodClamp=")
     << d.minLODClamp << lit(" bufOff=") << d.byteOffset << lit(" bufSize=") << d.byteSize
     << lit(" structCount=") << d.bufferStructCount << lit(" elemBytes=") << d.elementByteSize
     << lit("\n");
  if(d.resource != ResourceId())
  {
    ts << lit("           Resource: id=") << rdcToQString(ToStr(d.resource)) << lit(" \"")
       << rdcToQString(ctx.GetResourceName(d.resource)) << lit("\"");
    const TextureDescription *tex = ctx.GetTexture(d.resource);
    if(tex)
    {
      ts << lit("  TEXTURE storageFmt=") << rdcToQString(ToStr(tex->format)) << lit(" size=")
         << tex->width << lit("x") << tex->height << lit("x") << tex->depth << lit(" mips=")
         << tex->mips << lit(" arraysize=") << tex->arraysize << lit(" dim=") << tex->dimension
         << lit(" type=") << rdcToQString(ToStr(tex->type));
    }
    else if(const BufferDescription *buf = ctx.GetBuffer(d.resource))
    {
      ts << lit("  BUFFER length=") << buf->length;
    }
    ts << lit("\n");
  }
  if(d.secondary != ResourceId())
  {
    ts << lit("           Secondary: id=") << rdcToQString(ToStr(d.secondary)) << lit(" \"")
       << rdcToQString(ctx.GetResourceName(d.secondary)) << lit("\"\n");
  }
}

static rdcstr collectShaderDisassembly(ICaptureContext &ctx, IReplayController *r)
{
  rdcstr ret;
  if(!r)
    return ret;

  const PipeState &pipe = ctx.CurPipelineState();
  if(!pipe.IsCaptureLoaded())
    return ret;

  ResourceId pipeObj = pipe.GetGraphicsPipelineObject();
  if(pipeObj == ResourceId())
    pipeObj = pipe.GetComputePipelineObject();

  const size_t kMaxDefaultDisasm = 100000;
  const size_t kMaxAltDisasm = 48000;
  const size_t kMaxExtraTargets = 2;

  rdcarray<rdcstr> targets = r->GetDisassemblyTargets(pipeObj != ResourceId());

  for(uint8_t s = 0; s < (uint8_t)ShaderStage::Count; s++)
  {
    ShaderStage st = (ShaderStage)s;
    const ShaderReflection *refl = pipe.GetShaderReflection(st);
    if(!refl || refl->resourceId == ResourceId())
      continue;

    ret += "=== Shader stage ";
    ret += ToStr(st);
    ret += " id=";
    ret += ToStr(refl->resourceId);
    ret += " ===\n";
    appendShaderReflectionBindingSummary(ret, refl);
    ret += "\n";

    {
      rdcstr disasm = r->DisassembleShader(pipeObj, refl, rdcstr());
      if(disasm.size() > kMaxDefaultDisasm)
      {
        disasm.resize(kMaxDefaultDisasm);
        disasm += "\n... (default disassembly truncated)\n";
      }
      ret += "--- Default disassembly ---\n";
      ret += disasm;
      ret += "\n";
    }

    size_t extras = 0;
    // Index 0 is usually the same as DisassembleShader(..., "") (e.g. D3D12 "DXBC/DXIL"); start at 1
    for(size_t ti = 1; ti < targets.size() && extras < kMaxExtraTargets; ti++)
    {
      const rdcstr &target = targets[ti];

      rdcstr disasm = r->DisassembleShader(pipeObj, refl, target);
      if(disasm.empty() || disasm.size() < 32)
        continue;
      if(disasm.contains("Invalid Shader"))
        continue;

      if(disasm.size() > kMaxAltDisasm)
      {
        disasm.resize(kMaxAltDisasm);
        disasm += "\n... (alternate disassembly truncated)\n";
      }
      ret += "--- Disassembly target: ";
      ret += target;
      ret += " ---\n";
      ret += disasm;
      ret += "\n";
      extras++;
    }

    ret += "\n";
  }

  if(ret.empty())
    ret = "(No shader reflection / disassembly available at this event.)\n";

  return ret;
}

static QString systemPrompt()
{
  return lit(
      "You are an expert GPU graphics debugger helping a user inside RenderDoc.\n"
      "You receive a structured text dump of the capture and current event. The dump begins with "
      "\"Context data skills\" listing which sections were included based on the user's question "
      "(keywords route to blend, OM targets, descriptors, shaders, geometry, etc.; an empty question "
      "requests all sections).\n"
      "When present, sections may include: API metadata, action parameters, marker/event hierarchy, "
      "viewports/scissors, index/vertex buffers, vertex layout, render targets and depth, full per-RT "
      "color/alpha blend equations, stencil summary, per-stage descriptors with texture/buffer details "
      "(view format, mips, slices, swizzle, backing sizes) and sampler state (filter, address, LOD, "
      "compare), descriptor access, shader messages, a brief bound-resource list, a capture-wide "
      "texture/buffer/resource registry (dimensions, formats, categories; not raw texels or buffer "
      "bytes), plus optional multi-target shader disassembly appended separately when enabled.\n"
      "Explain clearly, cite which draw/dispatch or shader stage you refer to, and avoid guessing "
      "when information is missing.");
}

static QByteArray makeOpenAIChatPayload(const QString &model, const QString &userBody)
{
  QJsonObject root;
  root[lit("model")] = model;
  QJsonArray msgs;
  QJsonObject s;
  s[lit("role")] = lit("system");
  s[lit("content")] = systemPrompt();
  msgs.append(s);
  QJsonObject u;
  u[lit("role")] = lit("user");
  u[lit("content")] = userBody;
  msgs.append(u);
  root[lit("messages")] = msgs;
  return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

static QString openAIMessageContentToString(const QJsonValue &contentVal, QString &err)
{
  if(contentVal.isString())
  {
    QString s = contentVal.toString();
    if(!s.isEmpty())
      return s;
  }
  else if(contentVal.isArray())
  {
    QString pieces;
    for(const QJsonValue &part : contentVal.toArray())
    {
      QJsonObject o = part.toObject();
      if(o[lit("type")].toString() == lit("text"))
        pieces += o[lit("text")].toString();
    }
    if(!pieces.isEmpty())
      return pieces;
  }
  err = lit("No message content in response");
  return QString();
}

static QString extractOpenAIStyleText(const QJsonObject &root, QString &err)
{
  err.clear();
  if(root.contains(lit("error")))
  {
    QJsonObject e = root[lit("error")].toObject();
    err = e[lit("message")].toString();
    if(err.isEmpty())
      err = lit("Unknown API error");
    return QString();
  }
  QJsonArray choices = root[lit("choices")].toArray();
  if(choices.isEmpty())
  {
    err = lit("Empty choices in response");
    return QString();
  }
  QJsonObject c0 = choices[0].toObject();
  QJsonObject msg = c0[lit("message")].toObject();
  return openAIMessageContentToString(msg[lit("content")], err);
}

static QString extractAnthropicText(const QJsonObject &root, QString &err)
{
  if(root.contains(lit("error")))
  {
    QJsonObject e = root[lit("error")].toObject();
    err = e[lit("message")].toString();
    if(err.isEmpty())
      err = lit("Anthropic API error");
    return QString();
  }
  QJsonArray content = root[lit("content")].toArray();
  for(const QJsonValue &v : content)
  {
    QJsonObject blk = v.toObject();
    if(blk[lit("type")].toString() == lit("text"))
      return blk[lit("text")].toString();
  }
  err = lit("No text block in Anthropic response");
  return QString();
}

static QString extractGeminiText(const QJsonObject &root, QString &err)
{
  if(root.contains(lit("error")))
  {
    QJsonObject e = root[lit("error")].toObject();
    err = e[lit("message")].toString();
    return QString();
  }
  QJsonArray cands = root[lit("candidates")].toArray();
  if(cands.isEmpty())
  {
    err = lit("No candidates in Gemini response");
    return QString();
  }
  QJsonObject c0 = cands[0].toObject();
  QJsonObject cont = c0[lit("content")].toObject();
  QJsonArray parts = cont[lit("parts")].toArray();
  if(parts.isEmpty())
  {
    err = lit("No parts in Gemini response");
    return QString();
  }
  return parts[0].toObject()[lit("text")].toString();
}

AgentAssistantPanel::AgentAssistantPanel(ICaptureContext &ctx, QWidget *parent)
    : QFrame(parent), ui(new Ui::AgentAssistantPanel), m_Ctx(ctx)
{
  ui->setupUi(this);

  m_net = new QNetworkAccessManager(this);

  ui->providerCombo->addItem(lit("OpenAI"), (int)AgentLLMBackend::OpenAI);
  ui->providerCombo->addItem(lit("Anthropic"), (int)AgentLLMBackend::Anthropic);
  ui->providerCombo->addItem(lit("Google Gemini"), (int)AgentLLMBackend::GoogleGemini);
  ui->providerCombo->addItem(lit("Azure OpenAI"), (int)AgentLLMBackend::AzureOpenAI);
  ui->providerCombo->addItem(lit("OpenAI-compatible URL"), (int)AgentLLMBackend::OpenAICompatible);
  ui->providerCombo->addItem(lit("OpenRouter"), (int)AgentLLMBackend::OpenRouter);
  ui->providerCombo->addItem(lit("Zhipu GLM"), (int)AgentLLMBackend::GLM_Zhipu);
  ui->providerCombo->addItem(lit("GitHub Models"), (int)AgentLLMBackend::GitHubModels);

  m_activeLLMProvider = -1;
  loadSettingsFromConfig();
  m_activeLLMProvider = ui->providerCombo->currentIndex();
  updateProviderUi();

  QObject::connect(ui->providerCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                   &AgentAssistantPanel::onProviderChanged);
  QObject::connect(ui->copyPromptButton, &QPushButton::clicked, this, &AgentAssistantPanel::copyPrompt);
  QObject::connect(ui->copyContextButton, &QPushButton::clicked, this,
                   &AgentAssistantPanel::copyContextOnly);
  QObject::connect(ui->refreshButton, &QPushButton::clicked, this, &AgentAssistantPanel::refreshSnapshot);
  QObject::connect(ui->sendLLMButton, &QPushButton::clicked, this, &AgentAssistantPanel::sendToLLM);

  if(ui->questionEdit)
  {
    QObject::connect(ui->questionEdit, &QLineEdit::textChanged, this,
                     &AgentAssistantPanel::rebuildSnapshot);
  }

  m_Ctx.AddCaptureViewer(this);

  rebuildSnapshot();
}

AgentAssistantPanel::~AgentAssistantPanel()
{
  saveSettingsToConfig();
  m_Ctx.RemoveCaptureViewer(this);
  delete ui;
}

void AgentAssistantPanel::OnCaptureLoaded()
{
  rebuildSnapshot();
}

void AgentAssistantPanel::OnCaptureClosed()
{
  rebuildSnapshot();
}

void AgentAssistantPanel::OnSelectedEventChanged(uint32_t eventId)
{
  (void)eventId;
  rebuildSnapshot();
}

void AgentAssistantPanel::OnEventChanged(uint32_t eventId)
{
  (void)eventId;
  rebuildSnapshot();
}

void AgentAssistantPanel::rebuildSnapshot()
{
  ui->snapshotEdit->setPlainText(formatPipelineSnapshot());
}

QString AgentAssistantPanel::formatPipelineSnapshot()
{
  ICaptureContext &ctx = m_Ctx;

  if(!ctx.IsCaptureLoaded())
  {
    return lit("No capture loaded.\n\nOpen an .rdc file to populate this summary.");
  }

  QString ret;
  QTextStream ts(&ret);

  const QString questionForSkills = ui->questionEdit ? ui->questionEdit->text() : QString();
  const uint32_t skills = selectAgentDataSkills(questionForSkills);

  ts << lit("=== RenderDoc capture + event snapshot (for LLM) ===\n");
  ts << lit("Context data skills (from your question): ") << agentDataSkillMaskSummary(skills)
     << lit("\n");
  if(skills != AgentSkill_All)
  {
    ts << lit(
        "Tip: Mention blend, texture/binding/model/mesh, shader/disasm, vertex/index, marker/eid, "
        "depth/stencil, dispatch, printf, attachment, or catalog/列表 for capture-wide textures/buffers; "
        "clear the question for a full snapshot.\n");
  }
  if((skills & AgentSkill_Shaders) &&
     questionContainsAny(questionForSkills,
                         {"disasm", "disassemble", "assembly", "反编译", "汇编", "dxil", "spirv"}) &&
     ui->includeDisasmCheck && !ui->includeDisasmCheck->isChecked())
  {
    ts << lit(
        "[Tip: Enable \"Include shader disassembly\" to attach DisassembleShader output when sending to "
        "the LLM.]\n");
  }
  ts << lit("\n");

  const uint32_t kMaxVBuf = 32;
  const uint32_t kMaxVtxAttr = 48;
  const uint32_t kMaxViewport = 16;
  const uint32_t kMaxDescPerStage = 64;
  const uint32_t kMaxColorBlendRTs = 16;
  const uint32_t kMaxDescAccess = 96;
  const uint32_t kMaxShaderMsg = 24;
  const uint32_t kMaxResBrief = 64;

  if(skills & AgentSkill_CaptureMeta)
  {
    ts << lit("Capture file: ") << rdcToQString(ctx.GetCaptureFilename()) << lit("\n");

    const APIProperties &api = ctx.APIProps();
    ts << lit("Graphics API (captured): ") << ToQStr(api.pipelineType) << lit("\n");
    ts << lit("Local replay API: ") << ToQStr(api.localRenderer) << lit("\n");
    ts << lit("GPU vendor: ") << rdcToQString(ToStr(api.vendor)) << lit("  remoteReplay=")
       << (api.remoteReplay ? lit("true") : lit("false")) << lit("  degraded=")
       << (api.degraded ? lit("true") : lit("false")) << lit("  shaderDebugging=")
       << (api.shaderDebugging ? lit("true") : lit("false")) << lit("  pixelHistory=")
       << (api.pixelHistory ? lit("true") : lit("false")) << lit("\n");

    const FrameDescription &fi = ctx.FrameInfo();
    ts << lit("Frame number: ") << fi.frameNumber << lit("  captureUnixTimeUTC: ") << fi.captureTime
       << lit("  uncompressedCaptureBytes: ") << fi.uncompressedFileSize << lit("\n");
    if(fi.stats.recorded)
    {
      ts << lit("Frame statistics (API-specific; may be sparse): draws.calls=")
         << fi.stats.draws.calls << lit(" instanced=") << fi.stats.draws.instanced << lit(" indirect=")
         << fi.stats.draws.indirect << lit("  dispatches.calls=") << fi.stats.dispatches.calls
         << lit("\n");
    }
    ts << lit("Annotations in capture: ") << (fi.containsAnnotations ? lit("yes") : lit("no"))
       << lit("\n");
    ts << lit("Current event ID: ") << ctx.CurEvent() << lit("  Selected event ID: ")
       << ctx.CurSelectedEvent() << lit("\n");
    ts << lit("Textures in capture: ") << (uint32_t)ctx.GetTextures().size() << lit("  Buffers: ")
       << (uint32_t)ctx.GetBuffers().size() << lit("\n\n");
  }

  const ActionDescription *action = ctx.GetAction(ctx.CurEvent());
  if((skills & AgentSkill_Action) && action)
  {
    ts << lit("Current action (event ") << action->eventId << lit(" actionId ") << action->actionId
       << lit(")\n");
    ts << lit("  Name: ") << rdcToQString(action->GetName(ctx.GetStructuredFile())) << lit("\n");
    ts << lit("  Flags: ") << rdcToQString(ToStr(action->flags)) << lit("\n");
    ts << lit("  Draw: numIndices=") << action->numIndices << lit(" numInstances=")
       << action->numInstances << lit(" baseVertex=") << action->baseVertex << lit(" indexOffset=")
       << action->indexOffset << lit(" vertexOffset=") << action->vertexOffset
       << lit(" instanceOffset=") << action->instanceOffset << lit(" drawIndex=") << action->drawIndex
       << lit("\n");
    ts << lit("  Dispatch: groups xyz=") << action->dispatchDimension[0] << lit(" ")
       << action->dispatchDimension[1] << lit(" ") << action->dispatchDimension[2]
       << lit(" threadsPerGroup xyz=") << action->dispatchThreadsDimension[0] << lit(" ")
       << action->dispatchThreadsDimension[1] << lit(" ") << action->dispatchThreadsDimension[2]
       << lit(" baseGroup xyz=") << action->dispatchBase[0] << lit(" ") << action->dispatchBase[1]
       << lit(" ") << action->dispatchBase[2] << lit("\n");
    ts << lit("  Coarse outputs[0..7]: ");
    for(int i = 0; i < 8; i++)
      ts << rdcToQString(ToStr(action->outputs[i])) << lit(" ");
    ts << lit("\n  Coarse depthOut: ") << rdcToQString(ToStr(action->depthOut)) << lit("\n");

    ts << lit("  Marker / action hierarchy (root to current):\n");
    rdcarray<const ActionDescription *> chain;
    for(const ActionDescription *walk = action; walk; walk = walk->parent)
      chain.push_back(walk);
    for(size_t ci = chain.size(); ci > 0; ci--)
    {
      const ActionDescription *a = chain[ci - 1];
      ts << lit("    - EID ") << a->eventId << lit(": ")
         << rdcToQString(a->GetName(ctx.GetStructuredFile())) << lit("\n");
    }
    ts << lit("\n");
  }
  else if(skills & AgentSkill_Action)
  {
    ts << lit("No action matches the current event (still showing global/capture state).\n\n");
  }

  if(skills & AgentSkill_AssetCatalog)
    appendCaptureAssetCatalog(ts, ctx);

  const PipeState &pipe = ctx.CurPipelineState();
  if(pipe.IsCaptureLoaded())
  {
    const rdcarray<Descriptor> outs = pipe.GetOutputTargets();

    if(skills & AgentSkill_PipelineSummary)
    {
      ts << lit("Pipeline state (API-agnostic summary)\n");
      ts << lit("  Topology: ") << rdcToQString(ToStr(pipe.GetPrimitiveTopology())) << lit("\n");
      ts << lit("  Graphics PSO: ") << rdcToQString(ToStr(pipe.GetGraphicsPipelineObject())) << lit("\n");
      ts << lit("  Compute PSO: ") << rdcToQString(ToStr(pipe.GetComputePipelineObject())) << lit("\n");
      ts << lit("  Tessellation enabled: ")
         << (pipe.IsTessellationEnabled() ? lit("yes") : lit("no"))
         << lit("  MultiviewBroadcastCount: ") << pipe.MultiviewBroadcastCount()
         << lit("  RasterizedStream: ") << pipe.GetRasterizedStream() << lit("\n");
      ts << lit("  SupportsResourceArrays: ") << (pipe.SupportsResourceArrays() ? lit("yes") : lit("no"))
         << lit("  SupportsBarriers: ") << (pipe.SupportsBarriers() ? lit("yes") : lit("no"))
         << lit("\n\n");
    }

    if(skills & AgentSkill_ViewGeom)
    {
      ts << lit("Viewports (first ") << kMaxViewport << lit(")\n");
      for(uint32_t vi = 0; vi < kMaxViewport; vi++)
      {
        Viewport vp = pipe.GetViewport(vi);
        if(vp.width == 0 && vp.height == 0)
          continue;
        ts << lit("  [") << vi << lit("] x=") << vp.x << lit(" y=") << vp.y << lit(" w=") << vp.width
           << lit(" h=") << vp.height << lit(" minDepth=") << vp.minDepth << lit(" maxDepth=")
           << vp.maxDepth << lit("\n");
      }
      ts << lit("Scissors (first ") << kMaxViewport << lit(")\n");
      for(uint32_t si = 0; si < kMaxViewport; si++)
      {
        Scissor sc = pipe.GetScissor(si);
        if(sc.width == 0 && sc.height == 0)
          continue;
        ts << lit("  [") << si << lit("] x=") << sc.x << lit(" y=") << sc.y << lit(" w=") << sc.width
           << lit(" h=") << sc.height << lit("\n");
      }
      ts << lit("\n");

      ts << lit("Index buffer\n");
      {
        BoundVBuffer ib = pipe.GetIBuffer();
        ts << lit("  resource=") << rdcToQString(ToStr(ib.resourceId)) << lit(" \"")
           << rdcToQString(ctx.GetResourceName(ib.resourceId)) << lit("\" offset=") << ib.byteOffset
           << lit(" stride=") << ib.byteStride << lit(" size=") << ib.byteSize
           << lit(" primitiveRestart=") << (pipe.IsRestartEnabled() ? lit("on") : lit("off"))
           << lit(" restartIndex=") << pipe.GetRestartIndex() << lit("\n\n");
      }

      ts << lit("Vertex buffers (first ") << kMaxVBuf << lit(" slots)\n");
      {
        rdcarray<BoundVBuffer> vbs = pipe.GetVBuffers();
        for(uint32_t i = 0; i < kMaxVBuf && i < (uint32_t)vbs.size(); i++)
        {
          const BoundVBuffer &vb = vbs[i];
          if(vb.resourceId == ResourceId())
            continue;
          ts << lit("  [") << i << lit("] ") << rdcToQString(ToStr(vb.resourceId)) << lit(" \"")
             << rdcToQString(ctx.GetResourceName(vb.resourceId)) << lit("\" offset=") << vb.byteOffset
             << lit(" stride=") << vb.byteStride << lit(" size=") << vb.byteSize << lit("\n");
        }
      }
      ts << lit("\nVertex input attributes (first ") << kMaxVtxAttr << lit(")\n");
      {
        rdcarray<VertexInputAttribute> attrs = pipe.GetVertexInputs();
        for(uint32_t i = 0; i < kMaxVtxAttr && i < (uint32_t)attrs.size(); i++)
        {
          const VertexInputAttribute &va = attrs[i];
          ts << lit("  [") << i << lit("] vbIndex=") << va.vertexBuffer << lit(" offset=") << va.byteOffset
             << lit(" perInstance=") << (va.perInstance ? lit("yes") : lit("no"))
             << lit(" instanceRate=") << va.instanceRate << lit(" fmt=")
             << rdcToQString(ToStr(va.format)) << lit(" name=\"") << rdcToQString(va.name) << lit("\"\n");
        }
      }
      ts << lit("\n");
    }

    if(skills & AgentSkill_Shaders)
    {
      ts << lit("Bound shaders\n");
      for(uint8_t s = 0; s < (uint8_t)ShaderStage::Count; s++)
      {
        ShaderStage st = (ShaderStage)s;
        ResourceId sh = pipe.GetShader(st);
        if(sh != ResourceId())
        {
          ts << lit("  ") << rdcToQString(ToStr(st)) << lit(": id=") << rdcToQString(ToStr(sh))
             << lit(" name=\"") << rdcToQString(ctx.GetResourceName(sh)) << lit("\" entry=")
             << rdcToQString(pipe.GetShaderEntryPoint(st)) << lit("\n");
        }
      }
      ts << lit("\n");
    }

    if(skills & AgentSkill_OMTargets)
    {
      ts << lit("Output merger: color RTVs (render targets)\n");
      for(size_t i = 0; i < outs.size(); i++)
      {
        if(outs[i].resource == ResourceId())
          continue;
        ts << lit("  [") << (uint32_t)i << lit("] ") << rdcToQString(ToStr(outs[i].resource)) << lit(" \"")
           << rdcToQString(ctx.GetResourceName(outs[i].resource)) << lit("\" descType=")
           << rdcToQString(ToStr(outs[i].type)) << lit(" viewFmt=")
           << rdcToQString(ToStr(outs[i].format)) << lit(" texType=")
           << rdcToQString(ToStr(outs[i].textureType)) << lit(" view=")
           << rdcToQString(ToStr(outs[i].view)) << lit(" mips[") << (uint32_t)outs[i].firstMip
           << lit("+") << (uint32_t)outs[i].numMips << lit("] slices[") << outs[i].firstSlice
           << lit("+") << outs[i].numSlices << lit("] swizzle=")
           << rdcToQString(ToStr(outs[i].swizzle)) << lit("\n");
      }
      ts << lit("Depth-stencil target\n");
      {
        Descriptor ds = pipe.GetDepthTarget();
        if(ds.resource != ResourceId())
        {
          ts << lit("  ") << rdcToQString(ToStr(ds.resource)) << lit(" \"")
             << rdcToQString(ctx.GetResourceName(ds.resource)) << lit("\" descType=")
             << rdcToQString(ToStr(ds.type)) << lit(" viewFmt=") << rdcToQString(ToStr(ds.format))
             << lit(" mips[") << (uint32_t)ds.firstMip << lit("+") << (uint32_t)ds.numMips
             << lit("] slices[") << ds.firstSlice << lit("+") << ds.numSlices << lit("]\n");
        }
        else
        {
          ts << lit("  (none)\n");
        }
        Descriptor dsr = pipe.GetDepthResolveTarget();
        if(dsr.resource != ResourceId())
        {
          ts << lit("Depth resolve: ") << rdcToQString(ToStr(dsr.resource)) << lit(" \"")
             << rdcToQString(ctx.GetResourceName(dsr.resource)) << lit("\"\n");
        }
      }
    }

    if(skills & AgentSkill_BlendStencil)
    {
      ts << lit("\nColor blend state (per render target, including RGB/A factors and ops)\n");
      ts << lit("  independentBlending=")
         << (pipe.IsIndependentBlendingEnabled() ? lit("yes") : lit("no")) << lit("\n");
      {
        rdcarray<ColorBlend> blends = pipe.GetColorBlends();
        for(size_t i = 0; i < blends.size() && i < kMaxColorBlendRTs; i++)
        {
          const ColorBlend &b = blends[i];
          ts << lit("  RTV[") << (uint32_t)i << lit("] blendEnabled=")
             << (b.enabled ? lit("yes") : lit("no")) << lit(" writeMask=0x")
             << QString::number(b.writeMask, 16) << lit(" logicOpEnabled=")
             << (b.logicOperationEnabled ? lit("yes") : lit("no")) << lit(" logicOp=")
             << rdcToQString(ToStr(b.logicOperation)) << lit("\n");
          ts << lit("           color: op=") << rdcToQString(ToStr(b.colorBlend.operation))
             << lit(" src=") << rdcToQString(ToStr(b.colorBlend.source)) << lit(" dst=")
             << rdcToQString(ToStr(b.colorBlend.destination)) << lit("\n");
          ts << lit("           alpha: op=") << rdcToQString(ToStr(b.alphaBlend.operation))
             << lit(" src=") << rdcToQString(ToStr(b.alphaBlend.source)) << lit(" dst=")
             << rdcToQString(ToStr(b.alphaBlend.destination)) << lit("\n");
        }
        if(blends.size() > kMaxColorBlendRTs)
          ts << lit("  ... ") << ((uint32_t)blends.size() - kMaxColorBlendRTs)
             << lit(" more RT blend slots omitted\n");
      }

      ts << lit("\nStencil faces (front then back): ref compareMask writeMask function\n");
      {
        rdcpair<StencilFace, StencilFace> sten = pipe.GetStencilFaces();
        ts << lit("  front: ref=") << sten.first.reference << lit(" cmpMask=0x")
           << QString::number(sten.first.compareMask, 16) << lit(" wrMask=0x")
           << QString::number(sten.first.writeMask, 16) << lit(" ")
           << rdcToQString(ToStr(sten.first.function)) << lit("\n");
        ts << lit("  back:  ref=") << sten.second.reference << lit(" cmpMask=0x")
           << QString::number(sten.second.compareMask, 16) << lit(" wrMask=0x")
           << QString::number(sten.second.writeMask, 16) << lit(" ")
           << rdcToQString(ToStr(sten.second.function)) << lit("\n");
      }
    }

    if(skills & AgentSkill_Descriptors)
    {
      ts << lit("\nPer-stage descriptors (onlyUsed=true; CB / read-only inputs / UAVs / samplers).\n");
      ts << lit(
          "Read-only entries include input textures and buffers (SRV, sampled image, etc.): view "
          "format, mip/slice range, swizzle, and backing texture dimensions/storage format.\n");
      for(uint8_t s = 0; s < (uint8_t)ShaderStage::Count; s++)
      {
        ShaderStage st = (ShaderStage)s;
        rdcarray<UsedDescriptor> cbs = pipe.GetConstantBlocks(st, true);
        rdcarray<UsedDescriptor> ros = pipe.GetReadOnlyResources(st, true);
        rdcarray<UsedDescriptor> rws = pipe.GetReadWriteResources(st, true);
        rdcarray<UsedDescriptor> samps = pipe.GetSamplers(st, true);
        if(cbs.empty() && ros.empty() && rws.empty() && samps.empty())
          continue;
        ts << lit("  ") << rdcToQString(ToStr(st)) << lit(": CBs=") << (uint32_t)cbs.size() << lit(" RO=")
           << (uint32_t)ros.size() << lit(" RW=") << (uint32_t)rws.size() << lit(" Samplers=")
           << (uint32_t)samps.size() << lit("\n");
        auto dumpList = [&](const QString &label, const rdcarray<UsedDescriptor> &list,
                            bool isSamplerList) {
          if(list.empty())
            return;
          ts << lit("    ") << label << lit(":\n");
          for(uint32_t i = 0; i < kMaxDescPerStage && i < (uint32_t)list.size(); i++)
          {
            const UsedDescriptor &ud = list[i];
            ts << lit("      [") << i << lit("] accessType=") << rdcToQString(ToStr(ud.access.type))
               << lit(" stage=") << rdcToQString(ToStr(ud.access.stage)) << lit(" bindIdx=")
               << ud.access.index << lit(" arrayEl=") << ud.access.arrayElement << lit(" unused=")
               << (ud.access.staticallyUnused ? lit("yes") : lit("no")) << lit("\n");
            if(isSamplerList)
            {
              appendSamplerDetailLine(ts, ctx, ud.sampler);
            }
            else
            {
              appendDescriptorDetailLine(ts, ctx, ud.descriptor);
              appendSamplerDetailLine(ts, ctx, ud.sampler);
            }
          }
          if(list.size() > kMaxDescPerStage)
            ts << lit("      ... ") << ((uint32_t)list.size() - kMaxDescPerStage) << lit(" more\n");
        };
        dumpList(lit("CB (constant buffers)"), cbs, false);
        dumpList(lit("RO (textures, buffers, input attachments, etc.)"), ros, false);
        dumpList(lit("RW (UAVs / image stores)"), rws, false);
        dumpList(lit("SMP (sampler objects only)"), samps, true);
      }
    }

    if(skills & AgentSkill_DescriptorAccess)
    {
      ts << lit("\nDescriptor access records (first ") << kMaxDescAccess << lit(")\n");
      {
        const rdcarray<DescriptorAccess> &acc = pipe.GetDescriptorAccess();
        ts << lit("  total=") << (uint32_t)acc.size() << lit("\n");
        for(uint32_t i = 0; i < kMaxDescAccess && i < (uint32_t)acc.size(); i++)
        {
          const DescriptorAccess &a = acc[i];
          ts << lit("  [") << i << lit("] ") << rdcToQString(ToStr(a.stage)) << lit(" ")
             << rdcToQString(ToStr(a.type)) << lit(" idx=") << a.index << lit(" arrayEl=")
             << a.arrayElement << lit(" store=") << rdcToQString(ToStr(a.descriptorStore))
             << lit(" unused=") << (a.staticallyUnused ? lit("yes") : lit("no")) << lit("\n");
        }
        if(acc.size() > kMaxDescAccess)
          ts << lit("  ... ") << ((uint32_t)acc.size() - kMaxDescAccess) << lit(" more\n");
      }
    }

    if(skills & AgentSkill_ShaderMessages)
    {
      ts << lit("\nShader printf / messages (first ") << kMaxShaderMsg << lit(")\n");
      {
        const rdcarray<ShaderMessage> &msgs = pipe.GetShaderMessages();
        if(msgs.empty())
          ts << lit("  (none)\n");
        for(uint32_t i = 0; i < kMaxShaderMsg && i < (uint32_t)msgs.size(); i++)
        {
          const ShaderMessage &m = msgs[i];
          ts << lit("  [") << i << lit("] ") << rdcToQString(ToStr(m.stage)) << lit(" line=")
             << m.disassemblyLine << lit(" ") << rdcToQString(m.message) << lit("\n");
        }
      }
    }

    if(skills & AgentSkill_ResourceBrief)
    {
      ts << lit("\nRender targets + bound input/output textures/buffers (brief, first ") << kMaxResBrief
         << lit(" unique ids)\n");
      {
        rdcarray<ResourceId> seen;
        auto noteRes = [&](ResourceId id) {
          if(id == ResourceId())
            return;
          for(ResourceId s : seen)
            if(s == id)
              return;
          if((uint32_t)seen.size() >= kMaxResBrief)
            return;
          seen.push_back(id);
        };
        for(const Descriptor &d : outs)
          noteRes(d.resource);
        noteRes(pipe.GetDepthTarget().resource);
        for(uint8_t s = 0; s < (uint8_t)ShaderStage::Count; s++)
        {
          ShaderStage st = (ShaderStage)s;
          for(const UsedDescriptor &ud : pipe.GetReadOnlyResources(st, true))
            noteRes(ud.descriptor.resource);
          for(const UsedDescriptor &ud : pipe.GetReadWriteResources(st, true))
            noteRes(ud.descriptor.resource);
        }
        for(ResourceId id : seen)
        {
          const TextureDescription *tex = ctx.GetTexture(id);
          if(tex)
          {
            ts << lit("  T ") << rdcToQString(ToStr(id)) << lit(" \"")
               << rdcToQString(ctx.GetResourceName(tex->resourceId)) << lit("\" ") << tex->width
               << lit("x") << tex->height << lit("x") << tex->depth << lit(" mips=") << tex->mips
               << lit(" arraysize=") << tex->arraysize << lit(" fmt=")
               << rdcToQString(ToStr(tex->format)) << lit("\n");
          }
          else
          {
            const BufferDescription *buf = ctx.GetBuffer(id);
            if(buf)
            {
              ts << lit("  B ") << rdcToQString(ToStr(id)) << lit(" \"")
                 << rdcToQString(ctx.GetResourceName(buf->resourceId)) << lit("\" length=") << buf->length
                 << lit("\n");
            }
            else
            {
              ts << lit("  ? ") << rdcToQString(ToStr(id)) << lit(" \"")
                 << rdcToQString(ctx.GetResourceName(id)) << lit("\"\n");
            }
          }
        }
      }
    }
  }

  ts << lit(
      "\nNote: long lists and shader dumps are truncated to keep requests within model context "
      "windows; use Refresh, narrower events, or a more specific question (see Context data skills) for "
      "more detail.\n");
  ts << lit("=== End snapshot ===\n");

  const int kMaxSnapshotChars = 280000;
  if(ret.size() > kMaxSnapshotChars)
  {
    ret.truncate(kMaxSnapshotChars);
    ret += lit(
        "\n\n[Snapshot truncated at character limit; move to a specific draw/dispatch or ask a "
        "narrower question.]\n");
  }

  return ret;
}

void AgentAssistantPanel::loadSettingsFromConfig()
{
  PersistantConfig &cfg = m_Ctx.Config();
  int p = cfg.AgentAssistant_LLMProvider;
  if(p < 0 || p > (int)AgentLLMBackend::GitHubModels)
    p = 0;
  ui->providerCombo->blockSignals(true);
  ui->providerCombo->setCurrentIndex(p);
  ui->providerCombo->blockSignals(false);

  ui->azureEndpointEdit->setText(rdcToQString(cfg.AgentAssistant_AzureEndpoint));
  ui->azureDeploymentEdit->setText(rdcToQString(cfg.AgentAssistant_AzureDeployment));
  ui->compatibleBaseUrlEdit->setText(rdcToQString(cfg.AgentAssistant_CompatibleBaseUrl));
  ui->includeDisasmCheck->setChecked(cfg.AgentAssistant_IncludeShaderDisassembly);

  switch((AgentLLMBackend)p)
  {
    case AgentLLMBackend::OpenAI:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_OpenAIApiKey));
      break;
    case AgentLLMBackend::Anthropic:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_AnthropicApiKey));
      break;
    case AgentLLMBackend::GoogleGemini:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_GoogleApiKey));
      break;
    case AgentLLMBackend::AzureOpenAI:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_AzureApiKey));
      break;
    case AgentLLMBackend::OpenAICompatible:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_CompatibleApiKey));
      break;
    case AgentLLMBackend::OpenRouter:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_OpenRouterApiKey));
      break;
    case AgentLLMBackend::GLM_Zhipu:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_GLMApiKey));
      break;
    case AgentLLMBackend::GitHubModels:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_GitHubModelsApiKey));
      break;
  }

  repopulateModelCombo();
}

void AgentAssistantPanel::saveSettingsToConfig()
{
  PersistantConfig &cfg = m_Ctx.Config();
  const int p = ui->providerCombo->currentIndex();
  const QString modelLine = ui->modelCombo->currentText().trimmed();

  cfg.AgentAssistant_LLMProvider = p;
  cfg.AgentAssistant_AzureEndpoint = qstrToRdc(ui->azureEndpointEdit->text().trimmed());
  cfg.AgentAssistant_AzureDeployment = qstrToRdc(ui->azureDeploymentEdit->text().trimmed());
  cfg.AgentAssistant_CompatibleBaseUrl = qstrToRdc(ui->compatibleBaseUrlEdit->text().trimmed());
  cfg.AgentAssistant_IncludeShaderDisassembly = ui->includeDisasmCheck->isChecked();

  switch((AgentLLMBackend)p)
  {
    case AgentLLMBackend::OpenAI:
      cfg.AgentAssistant_OpenAIApiKey = qstrToRdc(ui->apiTokenEdit->text());
      cfg.AgentAssistant_OpenAIModel = qstrToRdc(modelLine);
      break;
    case AgentLLMBackend::Anthropic:
      cfg.AgentAssistant_AnthropicApiKey = qstrToRdc(ui->apiTokenEdit->text());
      cfg.AgentAssistant_AnthropicModel = qstrToRdc(modelLine);
      break;
    case AgentLLMBackend::GoogleGemini:
      cfg.AgentAssistant_GoogleApiKey = qstrToRdc(ui->apiTokenEdit->text());
      cfg.AgentAssistant_GoogleModel = qstrToRdc(modelLine);
      break;
    case AgentLLMBackend::AzureOpenAI:
      cfg.AgentAssistant_AzureApiKey = qstrToRdc(ui->apiTokenEdit->text());
      break;
    case AgentLLMBackend::OpenAICompatible:
      cfg.AgentAssistant_CompatibleApiKey = qstrToRdc(ui->apiTokenEdit->text());
      cfg.AgentAssistant_OpenAIModel = qstrToRdc(modelLine);
      break;
    case AgentLLMBackend::OpenRouter:
      cfg.AgentAssistant_OpenRouterApiKey = qstrToRdc(ui->apiTokenEdit->text());
      cfg.AgentAssistant_OpenRouterModel = qstrToRdc(modelLine);
      break;
    case AgentLLMBackend::GLM_Zhipu:
      cfg.AgentAssistant_GLMApiKey = qstrToRdc(ui->apiTokenEdit->text());
      cfg.AgentAssistant_GLMModel = qstrToRdc(modelLine);
      break;
    case AgentLLMBackend::GitHubModels:
      cfg.AgentAssistant_GitHubModelsApiKey = qstrToRdc(ui->apiTokenEdit->text());
      cfg.AgentAssistant_GitHubModelsModel = qstrToRdc(modelLine);
      break;
  }

  cfg.Save();
}

void AgentAssistantPanel::updateProviderUi()
{
  int idx = ui->providerCombo->currentIndex();
  bool azure = (idx == (int)AgentLLMBackend::AzureOpenAI);
  bool compat = (idx == (int)AgentLLMBackend::OpenAICompatible);

  ui->azureEndpointLabel->setVisible(azure);
  ui->azureEndpointEdit->setVisible(azure);
  ui->azureDeployLabel->setVisible(azure);
  ui->azureDeploymentEdit->setVisible(azure);

  ui->compatibleUrlLabel->setVisible(compat);
  ui->compatibleBaseUrlEdit->setVisible(compat);

  ui->modelLabel->setVisible(!azure);
  ui->modelCombo->setVisible(!azure);

  if(azure)
    ui->tokenLabel->setText(tr("Azure API key"));
  else if((AgentLLMBackend)idx == AgentLLMBackend::GoogleGemini)
    ui->tokenLabel->setText(tr("Gemini API key"));
  else if((AgentLLMBackend)idx == AgentLLMBackend::GitHubModels)
    ui->tokenLabel->setText(tr("GitHub PAT (models scope)"));
  else if((AgentLLMBackend)idx == AgentLLMBackend::GLM_Zhipu)
    ui->tokenLabel->setText(tr("GLM API key"));
  else
    ui->tokenLabel->setText(tr("API token"));
}

void AgentAssistantPanel::repopulateModelCombo()
{
  PersistantConfig &cfg = m_Ctx.Config();
  const int idx = ui->providerCombo->currentIndex();

  ui->modelCombo->blockSignals(true);
  ui->modelCombo->clear();

  QString saved;
  QStringList presets;

  switch((AgentLLMBackend)idx)
  {
    case AgentLLMBackend::OpenAI:
      presets << lit("gpt-4.1") << lit("gpt-4.1-mini") << lit("gpt-4.1-nano") << lit("gpt-4o")
              << lit("gpt-4o-mini") << lit("o4-mini") << lit("o3") << lit("o3-mini") << lit("o1")
              << lit("o1-mini");
      saved = rdcToQString(cfg.AgentAssistant_OpenAIModel);
      break;
    case AgentLLMBackend::Anthropic:
      presets << lit("claude-opus-4-6") << lit("claude-sonnet-4-6") << lit("claude-haiku-4-5")
              << lit("claude-haiku-4-5-20251001") << lit("claude-sonnet-4-5-20250929")
              << lit("claude-opus-4-5-20251101") << lit("claude-sonnet-4-20250514")
              << lit("claude-opus-4-20250514") << lit("claude-3-7-sonnet-20250219")
              << lit("claude-3-5-sonnet-20241022") << lit("claude-3-5-haiku-20241022");
      saved = rdcToQString(cfg.AgentAssistant_AnthropicModel);
      break;
    case AgentLLMBackend::GoogleGemini:
      presets << lit("gemini-2.5-pro") << lit("gemini-2.5-flash") << lit("gemini-2.5-flash-lite")
              << lit("gemini-2.0-flash") << lit("gemini-2.0-flash-lite") << lit("gemini-1.5-pro")
              << lit("gemini-1.5-flash");
      saved = rdcToQString(cfg.AgentAssistant_GoogleModel);
      break;
    case AgentLLMBackend::AzureOpenAI: break;
    case AgentLLMBackend::OpenAICompatible:
      presets << lit("gpt-4.1") << lit("gpt-4.1-mini") << lit("gpt-4o") << lit("gpt-4o-mini")
              << lit("o4-mini") << lit("o3-mini") << lit("deepseek-chat") << lit("deepseek-reasoner");
      saved = rdcToQString(cfg.AgentAssistant_OpenAIModel);
      break;
    case AgentLLMBackend::OpenRouter:
      presets << lit("anthropic/claude-opus-4.6") << lit("anthropic/claude-sonnet-4.6")
              << lit("anthropic/claude-haiku-4.5") << lit("anthropic/claude-3.7-sonnet")
              << lit("openai/gpt-4.1") << lit("openai/gpt-4.1-mini") << lit("openai/gpt-4o")
              << lit("openai/gpt-4o-mini") << lit("openai/o4-mini") << lit("google/gemini-2.5-pro")
              << lit("google/gemini-2.5-flash") << lit("deepseek/deepseek-chat-v3-0324")
              << lit("meta-llama/llama-3.3-70b-instruct")
              << lit("mistralai/mistral-large-2411") << lit("qwen/qwen-2.5-72b-instruct");
      saved = rdcToQString(cfg.AgentAssistant_OpenRouterModel);
      break;
    case AgentLLMBackend::GLM_Zhipu:
      presets << lit("glm-4.6") << lit("glm-4.5") << lit("glm-4-plus") << lit("glm-4-flash")
              << lit("glm-4-air") << lit("glm-4-airx") << lit("glm-4-long");
      saved = rdcToQString(cfg.AgentAssistant_GLMModel);
      break;
    case AgentLLMBackend::GitHubModels:
      presets << lit("openai/gpt-4.1") << lit("openai/gpt-4.1-mini") << lit("openai/gpt-4.1-nano")
              << lit("openai/gpt-4o") << lit("openai/gpt-4o-mini") << lit("openai/o4-mini")
              << lit("openai/o3") << lit("openai/o3-mini") << lit("mistralai/Mistral-Large-2411")
              << lit("mistralai/Mistral-Nemo-Instruct-2407")
              << lit("meta-llama/Llama-3.3-70B-Instruct") << lit("deepseek/DeepSeek-V3-0324")
              << lit("google/gemini-2.5-pro") << lit("google/gemini-2.5-flash");
      saved = rdcToQString(cfg.AgentAssistant_GitHubModelsModel);
      break;
  }

  ui->modelCombo->addItems(presets);
  if(!saved.isEmpty())
    ui->modelCombo->setCurrentText(saved);
  else if(ui->modelCombo->count() > 0)
    ui->modelCombo->setCurrentIndex(0);

  ui->modelCombo->setEditable(true);
  ui->modelCombo->blockSignals(false);
}

void AgentAssistantPanel::onProviderChanged(int idx)
{
  PersistantConfig &cfg = m_Ctx.Config();
  if(m_activeLLMProvider >= 0 && m_activeLLMProvider != idx)
  {
    QString tok = ui->apiTokenEdit->text();
    QString mdl = ui->modelCombo->currentText().trimmed();
    switch((AgentLLMBackend)m_activeLLMProvider)
    {
      case AgentLLMBackend::OpenAI:
        cfg.AgentAssistant_OpenAIApiKey = qstrToRdc(tok);
        cfg.AgentAssistant_OpenAIModel = qstrToRdc(mdl);
        break;
      case AgentLLMBackend::Anthropic:
        cfg.AgentAssistant_AnthropicApiKey = qstrToRdc(tok);
        cfg.AgentAssistant_AnthropicModel = qstrToRdc(mdl);
        break;
      case AgentLLMBackend::GoogleGemini:
        cfg.AgentAssistant_GoogleApiKey = qstrToRdc(tok);
        cfg.AgentAssistant_GoogleModel = qstrToRdc(mdl);
        break;
      case AgentLLMBackend::AzureOpenAI: cfg.AgentAssistant_AzureApiKey = qstrToRdc(tok); break;
      case AgentLLMBackend::OpenAICompatible:
        cfg.AgentAssistant_CompatibleApiKey = qstrToRdc(tok);
        cfg.AgentAssistant_OpenAIModel = qstrToRdc(mdl);
        break;
      case AgentLLMBackend::OpenRouter:
        cfg.AgentAssistant_OpenRouterApiKey = qstrToRdc(tok);
        cfg.AgentAssistant_OpenRouterModel = qstrToRdc(mdl);
        break;
      case AgentLLMBackend::GLM_Zhipu:
        cfg.AgentAssistant_GLMApiKey = qstrToRdc(tok);
        cfg.AgentAssistant_GLMModel = qstrToRdc(mdl);
        break;
      case AgentLLMBackend::GitHubModels:
        cfg.AgentAssistant_GitHubModelsApiKey = qstrToRdc(tok);
        cfg.AgentAssistant_GitHubModelsModel = qstrToRdc(mdl);
        break;
    }
    cfg.AgentAssistant_AzureEndpoint = qstrToRdc(ui->azureEndpointEdit->text().trimmed());
    cfg.AgentAssistant_AzureDeployment = qstrToRdc(ui->azureDeploymentEdit->text().trimmed());
    cfg.AgentAssistant_CompatibleBaseUrl = qstrToRdc(ui->compatibleBaseUrlEdit->text().trimmed());
  }

  cfg.AgentAssistant_LLMProvider = idx;
  cfg.AgentAssistant_AzureEndpoint = qstrToRdc(ui->azureEndpointEdit->text().trimmed());
  cfg.AgentAssistant_AzureDeployment = qstrToRdc(ui->azureDeploymentEdit->text().trimmed());
  cfg.AgentAssistant_CompatibleBaseUrl = qstrToRdc(ui->compatibleBaseUrlEdit->text().trimmed());
  cfg.Save();

  m_activeLLMProvider = idx;
  loadSettingsFromConfig();
  m_activeLLMProvider = idx;
  updateProviderUi();
}

void AgentAssistantPanel::setLLMUiBusy(bool busy)
{
  ui->sendLLMButton->setEnabled(!busy);
  ui->providerCombo->setEnabled(!busy);
  ui->apiTokenEdit->setEnabled(!busy);
  ui->modelCombo->setEnabled(!busy);
  ui->azureEndpointEdit->setEnabled(!busy);
  ui->azureDeploymentEdit->setEnabled(!busy);
  ui->compatibleBaseUrlEdit->setEnabled(!busy);
  ui->includeDisasmCheck->setEnabled(!busy);
}

void AgentAssistantPanel::copyPrompt()
{
  QString q = ui->questionEdit->text().trimmed();
  if(q.isEmpty())
  {
    QMessageBox::information(this, tr("Pipeline Agent"),
                             tr("Please enter a question before copying."));
    return;
  }

  QString clip = lit("You are helping analyse a GPU frame in RenderDoc.\n\n");
  clip += lit("Question:\n") + q + lit("\n\n");
  clip += lit("Context:\n") + ui->snapshotEdit->toPlainText();

  QGuiApplication::clipboard()->setText(clip);
  QMessageBox::information(
      this, tr("Pipeline Agent"),
      tr("Copied the question and pipeline context to the clipboard.\n"
         "Paste it into your AI assistant (for example Cursor chat)."));
}

void AgentAssistantPanel::copyContextOnly()
{
  QGuiApplication::clipboard()->setText(ui->snapshotEdit->toPlainText());
  QMessageBox::information(this, tr("Pipeline Agent"),
                           tr("Copied the pipeline context to the clipboard."));
}

void AgentAssistantPanel::refreshSnapshot()
{
  rebuildSnapshot();
}

void AgentAssistantPanel::sendToLLM()
{
  saveSettingsToConfig();

  QString question = ui->questionEdit->text().trimmed();
  if(question.isEmpty())
  {
    QMessageBox::information(this, tr("Pipeline Agent"), tr("Please enter a question."));
    return;
  }

  PersistantConfig &cfg = m_Ctx.Config();
  const int p = ui->providerCombo->currentIndex();
  QString apiKey = ui->apiTokenEdit->text().trimmed();
  if(apiKey.isEmpty() && p != (int)AgentLLMBackend::OpenAICompatible)
  {
    QMessageBox::warning(this, tr("Pipeline Agent"), tr("Please enter an API token."));
    return;
  }
  if(p == (int)AgentLLMBackend::OpenAICompatible && apiKey.isEmpty())
  {
    QMessageBox::warning(this, tr("Pipeline Agent"),
                         tr("Many OpenAI-compatible endpoints still require a Bearer token."));
  }

  QString snapshot = ui->snapshotEdit->toPlainText();
  QString shaderBlock;
  if(ui->includeDisasmCheck->isChecked() && m_Ctx.IsCaptureLoaded())
  {
    rdcstr disasm;
    m_Ctx.Replay().BlockInvoke([&](IReplayController *r) { disasm = collectShaderDisassembly(m_Ctx, r); });
    shaderBlock = rdcToQString(disasm);
  }

  QString userBody =
      snapshot + lit("\n\n") + shaderBlock + lit("\nUser question:\n") + question;
  const int kMaxUserBodyChars = 450000;
  if(userBody.size() > kMaxUserBodyChars)
  {
    userBody.truncate(kMaxUserBodyChars);
    userBody += lit("\n\n[Total message truncated for API/request size limits.]\n");
  }

  QUrl url;
  QByteArray payload;
  QNetworkRequest req;

  auto setJsonContent = [&req]() {
    req.setHeader(QNetworkRequest::ContentTypeHeader, lit("application/json"));
  };

  switch((AgentLLMBackend)p)
  {
    case AgentLLMBackend::OpenAI:
    {
      url = QUrl(lit("https://api.openai.com/v1/chat/completions"));
      payload = makeOpenAIChatPayload(rdcToQString(cfg.AgentAssistant_OpenAIModel), userBody);
      setJsonContent();
      req.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
      break;
    }
    case AgentLLMBackend::Anthropic:
    {
      url = QUrl(lit("https://api.anthropic.com/v1/messages"));
      QJsonObject root;
      root[lit("model")] = rdcToQString(cfg.AgentAssistant_AnthropicModel);
      root[lit("max_tokens")] = 8192;
      root[lit("system")] = systemPrompt();
      QJsonArray msgs;
      QJsonObject u;
      u[lit("role")] = lit("user");
      u[lit("content")] = userBody;
      msgs.append(u);
      root[lit("messages")] = msgs;
      payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
      setJsonContent();
      req.setRawHeader("x-api-key", apiKey.toUtf8());
      req.setRawHeader("anthropic-version", "2023-06-01");
      break;
    }
    case AgentLLMBackend::GoogleGemini:
    {
      QString mid = rdcToQString(cfg.AgentAssistant_GoogleModel).trimmed();
      if(mid.isEmpty())
        mid = lit("gemini-1.5-flash");
      url.setUrl(QStringLiteral("https://generativelanguage.googleapis.com/v1beta/models/%1:generateContent")
                     .arg(mid));
      QUrlQuery q;
      q.addQueryItem(lit("key"), apiKey);
      url.setQuery(q);

      QJsonObject root;
      QJsonArray contents;
      QJsonObject turn;
      turn[lit("role")] = lit("user");
      QJsonArray parts;
      QJsonObject ptxt;
      ptxt[lit("text")] = systemPrompt() + QStringLiteral("\n\n") + userBody;
      parts.append(ptxt);
      turn[lit("parts")] = parts;
      contents.append(turn);
      root[lit("contents")] = contents;
      payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
      setJsonContent();
      break;
    }
    case AgentLLMBackend::AzureOpenAI:
    {
      QString ep = rdcToQString(cfg.AgentAssistant_AzureEndpoint).trimmed();
      QString dep = rdcToQString(cfg.AgentAssistant_AzureDeployment).trimmed();
      while(ep.endsWith(QLatin1Char('/')))
        ep.chop(1);
      if(ep.isEmpty() || dep.isEmpty())
      {
        QMessageBox::warning(this, tr("Pipeline Agent"),
                             tr("Azure OpenAI requires endpoint and deployment name."));
        return;
      }
      url = QUrl(ep + lit("/openai/deployments/") + dep +
                 lit("/chat/completions?api-version=2024-02-15-preview"));
      payload = makeOpenAIChatPayload(dep, userBody);
      setJsonContent();
      req.setRawHeader("api-key", apiKey.toUtf8());
      break;
    }
    case AgentLLMBackend::OpenAICompatible:
    {
      QString base = rdcToQString(cfg.AgentAssistant_CompatibleBaseUrl).trimmed();
      while(base.endsWith(QLatin1Char('/')))
        base.chop(1);
      if(base.isEmpty())
        base = lit("https://api.openai.com");
      url = QUrl(base + lit("/v1/chat/completions"));
      payload =
          makeOpenAIChatPayload(rdcToQString(cfg.AgentAssistant_OpenAIModel).trimmed(), userBody);
      setJsonContent();
      if(!apiKey.isEmpty())
        req.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
      break;
    }
    case AgentLLMBackend::OpenRouter:
    {
      url = QUrl(lit("https://openrouter.ai/api/v1/chat/completions"));
      payload = makeOpenAIChatPayload(rdcToQString(cfg.AgentAssistant_OpenRouterModel).trimmed(),
                                      userBody);
      setJsonContent();
      req.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
      req.setRawHeader("HTTP-Referer", "https://renderdoc.org");
      req.setRawHeader("X-Title", "RenderDoc Pipeline Agent");
      break;
    }
    case AgentLLMBackend::GLM_Zhipu:
    {
      url = QUrl(lit("https://open.bigmodel.cn/api/paas/v4/chat/completions"));
      payload = makeOpenAIChatPayload(rdcToQString(cfg.AgentAssistant_GLMModel).trimmed(), userBody);
      setJsonContent();
      req.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
      break;
    }
    case AgentLLMBackend::GitHubModels:
    {
      url = QUrl(lit("https://models.github.ai/inference/chat/completions"));
      payload =
          makeOpenAIChatPayload(rdcToQString(cfg.AgentAssistant_GitHubModelsModel).trimmed(), userBody);
      setJsonContent();
      req.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
      req.setRawHeader("Accept", "application/vnd.github+json");
      req.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
      break;
    }
  }

  req.setUrl(url);
  req.setRawHeader("User-Agent", "RenderDoc-PipelineAgent/1.0");

  ui->replyEdit->setPlainText(tr("Waiting for response..."));
  setLLMUiBusy(true);

  QNetworkReply *reply = m_net->post(req, payload);
  QObject::connect(reply, &QNetworkReply::finished, this, &AgentAssistantPanel::onLLMFinished);
}

void AgentAssistantPanel::onLLMFinished()
{
  QNetworkReply *reply = qobject_cast<QNetworkReply *>(QObject::sender());
  setLLMUiBusy(false);
  if(!reply)
    return;

  reply->deleteLater();

  int p = ui->providerCombo->currentIndex();
  QString err;
  QString text;

  if(reply->error() != QNetworkReply::NoError)
  {
    err = reply->errorString() + lit("\n") + QString::fromUtf8(reply->readAll());
    ui->replyEdit->setPlainText(tr("HTTP error:\n") + err);
    return;
  }

  QByteArray body = reply->readAll();
  QJsonParseError jerr;
  QJsonDocument doc = QJsonDocument::fromJson(body, &jerr);
  if(!doc.isObject())
  {
    ui->replyEdit->setPlainText(tr("Invalid JSON response:\n") + QString::fromUtf8(body));
    return;
  }

  QJsonObject root = doc.object();

  switch((AgentLLMBackend)p)
  {
    case AgentLLMBackend::OpenAI:
    case AgentLLMBackend::AzureOpenAI:
    case AgentLLMBackend::OpenAICompatible:
    case AgentLLMBackend::OpenRouter:
    case AgentLLMBackend::GLM_Zhipu:
    case AgentLLMBackend::GitHubModels:
      text = extractOpenAIStyleText(root, err);
      break;
    case AgentLLMBackend::Anthropic: text = extractAnthropicText(root, err); break;
    case AgentLLMBackend::GoogleGemini: text = extractGeminiText(root, err); break;
  }

  if(!err.isEmpty() && text.isEmpty())
    ui->replyEdit->setPlainText(tr("API error:\n") + err + lit("\n\nRaw:\n") + QString::fromUtf8(body));
  else if(text.isEmpty())
    ui->replyEdit->setPlainText(tr("Empty reply:\n") + QString::fromUtf8(body));
  else
    ui->replyEdit->setPlainText(text);
}
