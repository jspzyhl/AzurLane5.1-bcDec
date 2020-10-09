
#include <stdio.h>
#include <fstream>
#include <iostream>
#include <windows.h>
#include <io.h>
#include <sys/stat.h>
#include <string>
#include <algorithm>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_arch.h"
#include "lj_buf.h"
#include "lj_lib.h"
#include "lj_bcdump.h"


static std::string remove_unnecessary_slashes(const std::string& path_str)
{
	size_t end_pos = path_str.find_last_not_of(R"(\/)");
	size_t begin_pos = path_str.find_first_not_of(R"(\/)");
	if ((begin_pos > path_str.length()) || (end_pos > path_str.length()))
	{
		return std::string();
	}
	else
	{
		return path_str.substr(begin_pos, end_pos - begin_pos + 1);
	}
}

static std::string append_path(const std::string& path_left, const std::string& path_right)
{
	return remove_unnecessary_slashes(path_left) + '\\' + remove_unnecessary_slashes(path_right);
}

static std::string get_filename_from_path(const std::string& _filepath)
{
	std::string _norm = remove_unnecessary_slashes(_filepath);
	size_t slash_pos = _norm.find_last_of(R"(\/)");
	if (slash_pos > _norm.length())
	{
		return _norm;
	}
	else
	{
		return _norm.substr(slash_pos + 1);
	}
}

static std::string get_filename_stem(std::string filename)
{
	std::string _normalized = remove_unnecessary_slashes(filename);
	size_t dot_pos = _normalized.find('.');
	if (dot_pos > _normalized.length())
	{
		return _normalized;
	}
	else
	{
		return _normalized.substr(0, dot_pos);
	}
}

static std::string get_parent_path(std::string file_path)
{
	std::string _norm = remove_unnecessary_slashes(file_path);
	size_t slash_pos = _norm.find_last_of(R"(\/)");
	if (slash_pos > _norm.length())
	{
		return _norm;
	}
	else
	{
		return _norm.substr(0, slash_pos);
	}
}

void mkd(const std::string& _dir_path)
{
	std::string cmd = R"(mkdir ")";
	cmd += _dir_path;
	cmd += "\"";
	system(cmd.c_str());
}


namespace EPathType
{
	enum Type
	{
		Invalid,
		Directory,
		Regular,
	};
}


static EPathType::Type stat_path(std::string _path)
{
	struct stat st;
	if (0 == stat(_path.c_str(), &st))
	{
		if (st.st_mode & S_IFDIR)
		{
			return EPathType::Directory;

		}
		else if (st.st_mode & S_IFREG)
		{
			return EPathType::Regular;
		}
	}
	return EPathType::Invalid;
}


static int writer_buf(lua_State *L, const void *p, size_t size, void *sb)
{
	lj_buf_putmem((SBuf *)sb, p, (MSize)size);
	UNUSED(L);
	return 0;
}

bool DecodeByteCode(lua_State* L, const char* _BCFilePath, const char* _OutputFilePath)
{
	bool bSuccess = false;
	size_t sz = 0;
	const char* decbuf = nullptr;
	std::ofstream ofs;

	luaL_loadfile(L, _BCFilePath);

	GCfunc *fn = lj_lib_checkfunc(L, 1);
	static int strip = 1;
	SBuf *sb = lj_buf_tmp_(L);
	L->top = L->base + 1;
	if (!isluafunc(fn) || lj_bcwrite(L, funcproto(fn), writer_buf, sb, strip))
	{
		lj_err_caller(L, LJ_ERR_STRDUMP);
		bSuccess = false;
		goto exit;
	}

	setstrV(L, L->top - 1, lj_buf_str(L, sb));
	lj_gc_check(L);

	decbuf = lua_tolstring(L, -1, &sz);
	ofs.open(_OutputFilePath, std::ios::binary | std::ios::trunc);
	ofs.write(decbuf, sz);
	ofs.close();
	bSuccess = true;

exit:
	lua_settop(L, 0);
	return bSuccess;
}

void DecFileTo_Impl(lua_State* L, const std::string& _In_filepath, const char* _OutputDir)
{
	std::string fn_stem = get_filename_stem(get_filename_from_path(_In_filepath));
	std::string OutPath = append_path(_OutputDir, fn_stem) + ".lj";
	DecodeByteCode(L, _In_filepath.c_str(), OutPath.c_str());
}

inline void DecSingle(lua_State* L, const char* _InputFilePath, const char* _OutputDir)
{
	mkd(_OutputDir);
	DecFileTo_Impl(L, _InputFilePath, _OutputDir);
}

void DecDirectory(lua_State* L, const char* _InputDir, const char* _OutputDir)
{
	mkd(_OutputDir);

	_finddata_t fileinf;
	long handle = _findfirst(append_path(_InputDir, "*").c_str(), &fileinf);
	do
	{
		if (fileinf.attrib != _A_SUBDIR)
		{
			std::cout << fileinf.name << std::endl;
			std::string file_path = append_path(_InputDir, fileinf.name);
			DecFileTo_Impl(L, file_path, _OutputDir);

		}
	} while (!_findnext(handle, &fileinf));
	_findclose(handle);
}


int main(int _argc, char **_argv)
{
	lua_State *L = lua_open();

	switch (_argc)
	{
	case 2:
	{
		auto st = stat_path(_argv[1]);
		if (st == EPathType::Invalid)
		{
			std::cout << "Invalid input path." << std::endl;
			break;
		}
		if (st == EPathType::Directory)
		{
			std::string outdir = append_path(_argv[1], "dec");
			DecDirectory(L, _argv[1], outdir.c_str());
		}
		else
		{
			std::string outdir = append_path(get_parent_path(_argv[1]), "dec");
			DecSingle(L, _argv[1], outdir.c_str());
		}

		break;
	}
	case 3:
	{
		auto st = stat_path(_argv[1]);
		if (st == EPathType::Invalid)
		{
			std::cout << "Invalid input path." << std::endl;
			break;
		}
		if (st == EPathType::Directory)
		{
			DecDirectory(L, _argv[1], _argv[2]);
		}
		else
		{
			DecSingle(L, _argv[1], _argv[2]);
		}
		break;
	}

	default:
	{
		std::cout << R"(Usage: bcDec "InputFilePath/InputDir" ["OutputDir"])" << std::endl;
	}
	break;
	}

	lua_close(L);
	return 0;
}