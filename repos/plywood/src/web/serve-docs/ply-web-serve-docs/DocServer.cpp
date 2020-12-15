/*------------------------------------
  ///\  Plywood C++ Framework
  \\\/  https://plywood.arc80.com/
------------------------------------*/
#include <ply-web-serve-docs/Core.h>
#include <ply-web-serve-docs/DocServer.h>
#include <pylon/Parse.h>
#include <pylon-reflect/Import.h>

namespace ply {
namespace web {

void dumpContents(StringWriter* sw, const Contents* node, ArrayView<const Contents*> expandTo) {
    bool isExpanded = false;
    if (expandTo && expandTo.back() == node) {
        isExpanded = true;
        expandTo = expandTo.shortenedBy(1);
    } else {
        expandTo = {};
    }

    if (node->linkDestination) {
        sw->format("<a href=\"{}\">", node->linkDestination);
    }
    if (node->children) {
        sw->format("<li class=\"caret{}\">", isExpanded ? StringView{" caret-down"} : StringView{});
    } else {
        *sw << "<li>";
    }
    *sw << "<span>" << fmt::XMLEscape{node->title} << "</span>";
    *sw << "</li>\n";
    if (node->linkDestination) {
        *sw << "</a>";
    }
    if (node->children) {
        sw->format("<ul class=\"nested{}\">\n", isExpanded ? StringView{" active"} : StringView{});
        for (const Contents* child : node->children) {
            dumpContents(sw, child, expandTo);
        }
        *sw << "</ul>\n";
    }
}

void DocServer::init(StringView dataRoot) {
    FileSystem* fs = FileSystem::native();

    this->dataRoot = dataRoot;
    this->contentsPath = NativePath::join(dataRoot, "contents.pylon");
    FileStatus contentsStatus = fs->getFileStatus(this->contentsPath);
    if (contentsStatus.result == FSResult::OK) {
        this->reloadContents();
        this->contentsModTime = contentsStatus.modificationTime;
    }
}

void populateContentsMap(HashMap<DocServer::ContentsTraits>& pathToContents, Contents* node) {
    pathToContents.insertOrFind(node->linkDestination)->node = node;
    for (Contents* child : node->children) {
        child->parent = node;
        populateContentsMap(pathToContents, child);
    }
}

void DocServer::reloadContents() {
    FileSystem* fs = FileSystem::native();

    String contentsPylon = fs->loadText(this->contentsPath, TextFormat::unixUTF8());
    if (fs->lastResult() != FSResult::OK) {
        // FIXME: Log an error here
        return;
    }

    Owned<pylon::Node> aRoot = pylon::Parser{}.parse(contentsPylon).root;
    if (!aRoot->isValid()) {
        // FIXME: Log an error here
        return;
    }

    pylon::importInto(TypedPtr::bind(&this->contents), aRoot);

    this->pathToContents = HashMap<ContentsTraits>{};
    for (Contents* node : this->contents) {
        populateContentsMap(this->pathToContents, node);
    }
}

void DocServer::serve(StringView requestPath, ResponseIface* responseIface) {
    FileSystem* fs = FileSystem::native();

    // Check if contents.pylon has been updated:
    FileStatus contentsStatus = fs->getFileStatus(this->contentsPath);
    if (contentsStatus.result == FSResult::OK) {
        if (contentsStatus.modificationTime != this->contentsModTime.load(MemoryOrder::Acquire)) {
            ply::LockGuard<ply::Mutex> guard{this->contentsMutex};
            if (contentsStatus.modificationTime !=
                this->contentsModTime.load(MemoryOrder::Relaxed)) {
                this->reloadContents();
            }
            this->contentsModTime = contentsStatus.modificationTime;
        }
    }

    if (!this->contents) {
        responseIface->respondGeneric(ResponseCode::InternalError);
        return;
    }

    // Load page
    if (NativePath::isAbsolute(requestPath)) {
        responseIface->respondGeneric(ResponseCode::NotFound);
        return;
    }
    String absPath = NativePath::join(this->dataRoot, "pages", requestPath);
    ExistsResult exists = FileSystem::native()->exists(absPath);
    if (exists == ExistsResult::Directory) {
        absPath = NativePath::join(absPath, "index.html");
    } else {
        absPath += ".html";
    }
    String pageHtml =
        fs->loadText(NativePath::join(this->dataRoot, "pages", absPath), TextFormat::unixUTF8());
    StringViewReader svr{pageHtml};
    String pageTitle = svr.readView<fmt::Line>().trim(isWhite);
    if (fs->lastResult() != FSResult::OK) {
        responseIface->respondGeneric(ResponseCode::NotFound);
        return;
    }

    // Figure out which TOC entries to expand
    Array<const Contents*> expandTo;
    {
        auto cursor = this->pathToContents.find(StringView{"/docs/"} + requestPath);
        if (cursor.wasFound()) {
            const Contents* node = cursor->node;
            while (node) {
                expandTo.append(node);
                node = node->parent;
            }
        }
    }

    OutStream* outs = responseIface->respondWithStream(ResponseCode::OK);
    StringWriter* sw = outs->strWriter();
    *sw << "Content-Type: text/html\r\n\r\n";
    sw->format(R"#(<!DOCTYPE html>
<html>
<head>
<title>{}</title>
)#",
               pageTitle);
    *sw << R"#(<link href="/static/stylesheet.css" rel="stylesheet" type="text/css" />
<script>
var highlighted = null;
function highlight(elementID) {
    if (highlighted) {
        highlighted.style.background = "";
    }
    highlighted = document.getElementById(elementID);
    if (highlighted) {
        highlighted.style.background = "#ffffa0";
    }
}
window.onload = function() { 
    highlight(location.hash.substr(1));
    var defTitles = document.getElementsByClassName("defTitle");
    for (var i = 0; i < defTitles.length; i++) {
        defTitles[i].onmouseenter = function(e) {
            var linkElems = e.target.getElementsByClassName("headerlink");
            for (var j = 0; j < linkElems.length; j++) {
                linkElems[j].style.visibility = "visible";
            }
        }
        defTitles[i].onmouseleave = function(e) {
            var linkElems = e.target.getElementsByClassName("headerlink");
            for (var j = 0; j < linkElems.length; j++) {
                linkElems[j].style.visibility = "hidden";
            }
        }
    }

    var toggler = document.getElementsByClassName("caret");
    for (var i = 0; i < toggler.length; i++) {
      toggler[i].addEventListener("click", function() {
        this.classList.toggle("caret-down");
        this.nextElementSibling.classList.toggle("active");
      });
    }
}
window.onhashchange = function() { 
    highlight(location.hash.substr(1));
}
</script>
</head>
<body>
  <div class="siteTitle">
    <a href="/"><img src="/static/logo.svg" id="logo"/></a>
    <a href="https://www.patreon.com/preshing"><img src="/static/patron-button.svg" id="patron"></a>
    <a href="https://github.com/arc80/plywood"><img src="/static/github-button.svg" id="github"></a>
  </div>
  <div class="sidebar">
      <div class="inner">
          <ul>
)#";
    for (const Contents* node : this->contents) {
        dumpContents(sw, node, expandTo.view());
    }
    sw->format(R"(
          </ul>
      </div>
  </div>
  <article class="content">
<h1>{}</h1>
)",
               pageTitle);
    *sw << svr.viewAvailable();
    *sw << R"(
  </article>
</body>
</html>
)";
}

} // namespace web
} // namespace ply
