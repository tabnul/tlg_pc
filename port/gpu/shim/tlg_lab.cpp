// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: Lab tab rows (tlg_lab.h). The menu and the game thread share them under one lock.
#include "tlg_lab.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace TlgLab {
namespace {
std::mutex lock;
std::vector<Row> rows;
std::string wind_game;
std::filesystem::file_time_type seen{};
std::chrono::steady_clock::time_point checked{};

// Rows with the same object and property are applied together, from one saved game value, so
// rows for different components of one vector do not undo each other. A value differing from
// our last write was set by the game (area change, cutscene): it becomes the new base. Without
// a moved row the base is written back once and the property is left alone. Colors have five
// parts (r g b a intensity); those are written as text, as the game's env scripts do. A row
// that fails (an unknown property) shows the error in its game column.
constexpr std::string_view Helpers = R"(
local LAB={}
local function Walk(name,path)
  local o,prop=FindObject(name)
  for k in path:gmatch('[^%.]+') do
    if prop then if o==nil then return end o=o[prop] end
    prop=k
  end
  if o~=nil then return o,prop end
end
local VALS={}
local function Val(i,s) if VALS[i]==s then return end VALS[i]=s local f=ls('--TLGVAL '..i..' '..s) if f then f() end end
local function Lab(i,name,path,mode,parts,v,on)
  local k=name..'|'..path local t=LAB[k]
  if not t then t={n=name,p=path,ops={}} LAB[k]=t LAB[#LAB+1]=t end
  t.ops[#t.ops+1]={i=i,m=mode,p=parts,v=v,on=on}
end
local function One(t)
  do
    local o,prop=Walk(t.n,t.p)
    local cur=o and o[prop]
    local kind=type(cur)
    if kind~='number' and kind~='userdata' and kind~='string' then
      for _,op in ipairs(t.ops) do Val(op.i,'(not found)') end
    else
      local key='lab|'..t.n..'|'..t.p
      local s=tostring(cur)
      local e=B[key]
      if not e or s~=e.w then e={b=kind=='number' and cur or Parts(cur),d=e and e.d} B[key]=e end
      local w,on
      if kind=='number' then w=e.b else
        local count=#e.b if kind=='userdata' and count<4 then count=4 end
        w={} for c=1,count do w[c]=e.b[c] or 0 end
      end
      for _,op in ipairs(t.ops) do
        local function F(x) if op.m=='%' then return x*op.v/100 elseif op.m=='+' then return x+op.v end return op.v end
        on=on or op.on
        if kind=='number' then
          Val(op.i,tostring(e.b)) if op.on then w=F(w) end
        else
          local g={} for _,c in ipairs(op.p or ALL) do g[#g+1]=tostring(e.b[c] or 0) if op.on then w[c]=F(w[c] or 0) end end
          Val(op.i,table.concat(g,' '))
        end
      end
      if on or e.d then
        local n=w
        if kind=='string' or (kind=='userdata' and #w>4) then local p={} for c=1,#w do p[c]=string.format('%g',w[c]) end n=table.concat(p,' ')
        elseif kind=='userdata' then n=Vector(w[1],w[2],w[3],w[4]) end
        if tostring(n)~=s then o[prop]=n end
        e.w=tostring(o[prop])
        e.d=on
      end
    end
  end
end
local function LabApply()
  for _,t in ipairs(LAB) do
    local ok,e=pcall(One,t)
    if not ok then for _,op in ipairs(t.ops) do Val(op.i,'error: '..tostring(e)) end end
  end
end
)";

std::string Trim(std::string text) {
    const size_t first = text.find_first_not_of(" \t\r");
    const size_t last = text.find_last_not_of(" \t\r");
    return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

// Re-reads the file when it changed, at most twice a second; rows keep their values when the
// label, object and property stay the same. Caller holds the lock.
void Reload() {
    static const char* path = std::getenv("TLG_LAB");
    const auto now = std::chrono::steady_clock::now();
    if (!path || now - checked < std::chrono::milliseconds(500)) return;
    checked = now;
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    if (error || time == seen) return;
    seen = time;
    std::vector<Row> loaded;
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> fields;
        for (size_t start = 0;;) {
            const size_t bar = line.find('|', start);
            fields.push_back(Trim(line.substr(start, bar - start)));
            if (bar == std::string::npos) break;
            start = bar + 1;
        }
        if (fields.size() < 6 || fields[3].empty() || std::string("%+=").find(fields[3][0]) == std::string::npos) {
            std::printf("Lab: skipped line \"%s\"\n", line.c_str());
            continue;
        }
        Row row;
        row.label = fields[0];
        row.object = fields[1];
        row.path = fields[2];
        row.mode = fields[3][0];
        for (const char* part = fields[3].c_str() + 1; *part;) {
            char* end = nullptr;
            const long index = std::strtol(part, &end, 10);
            if (end == part) {
                ++part;
                continue;
            }
            if (index >= 1 && index <= 8) row.parts.push_back(int(index));
            part = end;
        }
        row.lo = std::strtof(fields[4].c_str(), nullptr);
        row.hi = std::strtof(fields[5].c_str(), nullptr);
        row.format = fields.size() > 6 && !fields[6].empty() ? fields[6] : "%.2f";
        row.help = fields.size() > 7 ? fields[7] : "";
        row.value = row.mode == '%' ? 100.0f : row.mode == '+' ? 0.0f : row.lo;
        for (const Row& old : rows) {
            if (old.label == row.label && old.object == row.object && old.path == row.path) {
                row.value = old.value;
                row.active = old.active;
                row.game = old.game;
            }
        }
        loaded.push_back(std::move(row));
    }
    std::printf("Lab: %zu rows from %s\n", loaded.size(), path);
    rows = std::move(loaded);
}
} // namespace

std::vector<Row> Rows() {
    std::lock_guard guard(lock);
    Reload();
    return rows;
}

void Set(size_t index, float value) {
    std::lock_guard guard(lock);
    if (index < rows.size()) {
        rows[index].value = value;
        rows[index].active = true;
    }
}

void Reset(size_t index) {
    std::lock_guard guard(lock);
    if (index < rows.size()) {
        Row& row = rows[index];
        row.active = false;
        row.value = row.mode == '%' ? 100.0f : row.mode == '+' ? 0.0f : row.lo;
    }
}

void ResetAll() {
    std::lock_guard guard(lock);
    for (Row& row : rows) {
        row.active = false;
        row.value = row.mode == '%' ? 100.0f : row.mode == '+' ? 0.0f : row.lo;
    }
}

std::string WindGame() {
    std::lock_guard guard(lock);
    return wind_game;
}

std::string Lua(int wind_skip) {
    std::lock_guard guard(lock);
    Reload();
    std::string code(Helpers);
    char line[512];
    std::snprintf(line, sizeof(line),
                  "Lab(-1,'_default_env','skinningSettings.skinWindMaxFrameSkip','=',nil,%d,%s)\n",
                  wind_skip < 0 ? 1 : wind_skip, wind_skip < 0 ? "false" : "true");
    code += line;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        std::string parts = "nil";
        if (!row.parts.empty()) {
            parts = "{";
            for (const int part : row.parts) parts += std::to_string(part) + ",";
            parts.back() = '}';
        }
        std::snprintf(line, sizeof(line), "Lab(%zu,'%s','%s','%c',%s,%g,%s)\n", i, row.object.c_str(),
                      row.path.c_str(), row.mode, parts.c_str(), row.value, row.active ? "true" : "false");
        code += line;
    }
    return code;
}

void Report(std::string_view text) {
    const size_t space = text.find(' ');
    if (space == std::string_view::npos) return;
    const int index = std::atoi(std::string(text.substr(0, space)).c_str());
    std::string value(text.substr(space + 1));
    std::lock_guard guard(lock);
    if (index == -1) {
        wind_game = std::move(value);
    } else if (index >= 0 && size_t(index) < rows.size()) {
        rows[index].game = std::move(value);
    }
}

} // namespace TlgLab
