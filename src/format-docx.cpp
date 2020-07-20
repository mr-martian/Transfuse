/*
* Copyright (C) 2020 Tino Didriksen <mail@tinodidriksen.com>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "shared.hpp"
#include "formats.hpp"
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/xmlsave.h>
#include <unicode/ustring.h>
#include <unicode/regex.h>
#include <zip.h>
using namespace icu;

namespace Transfuse {

void docx_merge_wt(State& state, xmlDocPtr xml) {
	auto ctx = xmlXPathNewContext(xml);
	if (ctx == nullptr) {
		throw std::runtime_error("Could not create XPath context");
	}

	if (xmlXPathRegisterNs(ctx, XC("w"), XC("http://schemas.openxmlformats.org/wordprocessingml/2006/main")) != 0) {
		throw std::runtime_error("Could not register namespace w");
	}

	auto rs = xmlXPathNodeEval(reinterpret_cast<xmlNodePtr>(xml), XC("//w:p"), ctx);
	if (rs == nullptr) {
		xmlXPathFreeContext(ctx);
		throw std::runtime_error("Could not execute XPath search for w:p elements");
	}

	if (xmlXPathNodeSetIsEmpty(rs->nodesetval)) {
		xmlXPathFreeObject(rs);
		throw std::runtime_error("XPath found zero w:p elements");
	}

	state.begin();

	xmlString tag;
	xmlString tmp;
	xmlString content;
	auto buf = xmlBufferCreate();

	auto ns = rs->nodesetval;
	for (int i = 0; i < ns->nodeNr; ++i) {
		// First merge all sibling <w:r><w:t>...</w:t></w:r>
		auto ts = xmlXPathNodeEval(ns->nodeTab[i], XC(".//w:t"), ctx);
		if (ts == nullptr) {
			xmlXPathFreeContext(ctx);
			throw std::runtime_error("Could not execute XPath search");
		}

		if (xmlXPathNodeSetIsEmpty(ts->nodesetval) || ts->nodesetval->nodeNr == 1) {
			xmlXPathFreeObject(ts);
			continue;
		}

		for (int j = 0; j < ts->nodesetval->nodeNr; ++j) {
			auto node = ts->nodesetval->nodeTab[j];
			content = node->children->content ? node->children->content : XC("");
			xmlNodeSetContent(node, XC(TF_SENTINEL));

			auto bp = node->parent;
			xmlBufferEmpty(buf);
			auto sz = xmlNodeDump(buf, bp->doc, bp, 0, 0);
			tag.assign(buf->content, buf->content + sz);

			xmlChar_view type{ XC("text") };
			if (tag.find(XC("<w:b/>")) != xmlString::npos && tag.find(XC("<w:i/>")) != xmlString::npos) {
				type = XC("b+i");
			}
			else if (tag.find(XC("<w:b/>")) != xmlString::npos) {
				type = XC("b");
			}
			else if (tag.find(XC("<w:i/>")) != xmlString::npos) {
				type = XC("i");
			}

			auto s = tag.find(XC(TF_SENTINEL));
			tmp.assign(tag.begin() + PD(s) + 3, tag.end());
			tag.erase(s);
			auto hash = state.style(type, tag, tmp);

			tmp = XC(TFI_OPEN_B);
			tmp += type;
			tmp += ':';
			tmp += hash;
			tmp += TFI_OPEN_E;
			tmp += content;
			tmp += TFI_CLOSE;

			if (bp->prev && xmlStrcmp(bp->prev->name, XC("tf-text")) == 0) {
				content = bp->prev->children->content;
				content += tmp;
				xmlNodeSetContent(bp->prev->children, content.c_str());
			}
			else {
				auto nn = xmlNewNode(nullptr, XC("tf-text"));
				nn = xmlAddPrevSibling(bp, nn);
				xmlNodeSetContent(nn, tmp.c_str());
			}
			xmlUnlinkNode(bp);
			xmlFreeNode(bp);
		}
		xmlXPathFreeObject(ts);

		// Merge <w:hyperlink>...</w:hyperlink> into child <tf-text>
		auto hs = xmlXPathNodeEval(ns->nodeTab[i], XC(".//w:hyperlink"), ctx);
		if (hs == nullptr) {
			xmlXPathFreeContext(ctx);
			throw std::runtime_error("Could not execute XPath search");
		}

		if (xmlXPathNodeSetIsEmpty(hs->nodesetval)) {
			xmlXPathFreeObject(hs);
			continue;
		}

		for (int j = 0; j < hs->nodesetval->nodeNr; ++j) {
			auto node = hs->nodesetval->nodeTab[j];
			auto text = node->children;
			xmlUnlinkNode(text);
			xmlAddPrevSibling(node, text);

			xmlNodeSetContent(node, XC(TF_SENTINEL));

			xmlBufferEmpty(buf);
			auto sz = xmlNodeDump(buf, node->doc, node, 0, 0);
			tag.assign(buf->content, buf->content + sz);

			auto s = tag.find(XC(TF_SENTINEL));
			tmp.assign(tag.begin() + PD(s) + 3, tag.end());
			tag.erase(s);
			auto hash = state.style(XC("a"), tag, tmp);

			content = XC(TFI_OPEN_B);
			content += "a:";
			content += hash;
			content += TFI_OPEN_E;
			content += text->children->content ? text->children->content : XC("");
			content += TFI_CLOSE;

			xmlNodeSetContent(text->children, content.c_str());
			xmlUnlinkNode(node);
			xmlFreeNode(node);
		}
	}

	xmlBufferFree(buf);
	state.commit();
}

std::unique_ptr<DOM> extract_docx(State& state) {
	int e = 0;
	auto zip = zip_open("original", ZIP_RDONLY, &e);
	if (zip == nullptr) {
		throw std::runtime_error(concat("Could not open DOCX file: ", std::to_string(e)));
	}

	zip_stat_t stat{};
	if (zip_stat(zip, "word/document.xml", 0, &stat) != 0) {
		throw std::runtime_error("DOCX did not have word/document.xml");
	}
	if (stat.size == 0) {
		throw std::runtime_error("DOCX document.xml was empty");
	}

	auto zf = zip_fopen_index(zip, stat.index, 0);
	if (zf == nullptr) {
		throw std::runtime_error("Could not open DOCX document.xml");
	}

	std::string data(stat.size, 0);
	zip_fread(zf, &data[0], stat.size);
	zip_fclose(zf);

	zip_close(zip);

	auto udata = UnicodeString::fromUTF8(data);

	udata.findAndReplace(" encoding=\"UTF-8\"", " encoding=\"UTF-16\"");

	// Wipe chaff that's not relevant when translated, or simply superfluous
	udata.findAndReplace(" xml:space=\"preserve\"", "");
	udata.findAndReplace(" w:eastAsiaTheme=\"minorHAnsi\"", "");

	// Revision tracking information
	UnicodeString tmp;
	UErrorCode status = U_ZERO_ERROR;

	RegexMatcher rx_R(R"X( w:rsidR="[^"]+")X", 0, status);
	rx_R.reset(udata);
	tmp = rx_R.replaceAll("", status);
	std::swap(udata, tmp);

	RegexMatcher rx_RPr(R"X( w:rsidRPr="[^"]+")X", 0, status);
	rx_RPr.reset(udata);
	tmp = rx_RPr.replaceAll("", status);
	std::swap(udata, tmp);

	RegexMatcher rx_Del(R"X( w:rsidDel="[^"]+")X", 0, status);
	rx_Del.reset(udata);
	tmp = rx_Del.replaceAll("", status);
	std::swap(udata, tmp);

	// Other full-tag chaff, intentionally done after attributes because removing those may leave these tags empty
	RegexMatcher rx_lang(R"X(<w:lang(?=[ >])[^/>]+/>)X", 0, status);
	rx_lang.reset(udata);
	tmp = rx_lang.replaceAll("", status);
	std::swap(udata, tmp);

	udata.findAndReplace("<w:lastRenderedPageBreak/>", "");
	udata.findAndReplace("<w:color w:val=\"auto\"/>", "");
	udata.findAndReplace("<w:rFonts/>", "");
	udata.findAndReplace("<w:rFonts></w:rFonts>", "");
	udata.findAndReplace("<w:rPr></w:rPr>", "");
	udata.findAndReplace("<w:softHyphen/>", "");

	RegexMatcher rx_wt(R"X(</w:t>([^<>]+?)<w:t(?=[ >])[^>]*>)X", 0, status);
	rx_wt.reset(udata);
	tmp = rx_wt.replaceAll("", status);
	std::swap(udata, tmp);

	auto xml = xmlReadMemory(reinterpret_cast<const char*>(udata.getTerminatedBuffer()), SI(SZ(udata.length()) * sizeof(UChar)), "document.xml", "UTF-16", XML_PARSE_RECOVER | XML_PARSE_NONET);
	if (xml == nullptr) {
		throw std::runtime_error(concat("Could not parse document.xml: ", xmlLastError.message));
	}
	udata.remove();
	tmp.remove();

	docx_merge_wt(state, xml);

	auto dom = std::make_unique<DOM>(state, xml);
	dom->tags_parents_allow = make_xmlChars("tf-text", "w:t");
	dom->save_spaces();

	auto buf = xmlBufferCreate();
	auto obuf = xmlOutputBufferCreateBuffer(buf, nullptr);
	xmlSaveFileTo(obuf, xml, "UTF-8");
	data.assign(buf->content, buf->content + buf->use);
	xmlBufferFree(buf);
	cleanup_styles(data);

	auto b = data.rfind("</tf-text><tf-text>");
	while (b != std::string::npos) {
		data.erase(b, 19);
		b = data.rfind("</tf-text><tf-text>");
	}

	dom->xml.reset(xmlReadMemory(reinterpret_cast<const char*>(data.data()), SI(data.size()), "styled.xml", "UTF-8", XML_PARSE_RECOVER | XML_PARSE_NONET));
	if (dom->xml == nullptr) {
		throw std::runtime_error(concat("Could not parse styled XML: ", xmlLastError.message));
	}
	file_save("styled.xml", data);

	return dom;
}

std::string inject_docx(DOM& dom) {
	auto buf = xmlBufferCreate();
	auto obuf = xmlOutputBufferCreateBuffer(buf, nullptr);
	xmlSaveFileTo(obuf, dom.xml.get(), "UTF-8");
	std::string data(buf->content, buf->content + buf->use);
	xmlBufferFree(buf);

	auto udata = UnicodeString::fromUTF8(data);
	UnicodeString tmp;
	UErrorCode status = U_ZERO_ERROR;

	RegexMatcher rx_after_r(R"X((</w:t></w:r>)([^<>]+))X", 0, status);
	rx_after_r.reset(udata);
	tmp = rx_after_r.replaceAll("$2$1", status);
	std::swap(udata, tmp);

	RegexMatcher rx_after_a(R"X((</w:t></w:r></w:hyperlink>)([^<>]+))X", 0, status);
	rx_after_a.reset(udata);
	tmp = rx_after_a.replaceAll("$2$1", status);
	std::swap(udata, tmp);

	RegexMatcher rx_snip_empty(R"X(<w:r><w:t/></w:r>)X", 0, status);
	rx_snip_empty.reset(udata);
	tmp = rx_snip_empty.replaceAll("", status);
	std::swap(udata, tmp);

	RegexMatcher rx_snip_tf(R"X(</?tf-text>)X", 0, status);
	rx_snip_tf.reset(udata);
	tmp = rx_snip_tf.replaceAll("", status);
	std::swap(udata, tmp);

	RegexMatcher rx_xml_space(R"X(<w:t([ >]))X", 0, status);
	rx_xml_space.reset(udata);
	tmp = rx_xml_space.replaceAll("<w:t xml:space=\"preserve\"$1", status);
	std::swap(udata, tmp);

	data.clear();
	udata.toUTF8String(data);
	file_save("injected.xml", data);

	fs::copy("original", "injected.docx");

	int e = 0;
	auto zip = zip_open("injected.docx", 0, &e);
	if (zip == nullptr) {
		throw std::runtime_error(concat("Could not open DOCX file: ", std::to_string(e)));
	}

	auto src = zip_source_file(zip, "injected.xml", 0, 0);
	if (src == nullptr) {
		throw std::runtime_error("Could not open injected.xml");
	}

	if (zip_file_add(zip, "word/document.xml", src, ZIP_FL_OVERWRITE) < 0) {
		throw std::runtime_error("Could not replace word/document.xml");
	}

	zip_close(zip);

	return "injected.docx";
}

}