#!/usr/bin/env python3
from pathlib import Path

p = Path('src/context.cpp')
text = p.read_text()
old = '''    } else if (!this->externalSemaphoreFdSync_ && this->previousSourceCopySignalValid_) {
        gameRenderSemaphores2.emplace_back(this->passInfos.at((this->frameIdx - 1) % 8)
            .preCopySemaphores.at(1).handle());
    }
'''
new = '''    } else if (!this->externalSemaphoreFdSync_) {
        if (this->previousSourceCopySignalValid_)
            gameRenderSemaphores2.emplace_back(this->passInfos.at((this->frameIdx - 1) % 8)
                .preCopySemaphores.at(1).handle());
    }
'''
if text.count(old) != 1:
    raise SystemExit(f'expected one source-history fallback block, got {text.count(old)}')
p.write_text(text.replace(old, new, 1))
