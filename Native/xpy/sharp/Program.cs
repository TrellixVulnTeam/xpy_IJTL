﻿using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using XPython;

namespace sharp
{
    class Program
    {
        static void Main(string[] args)
        {
            // TODO: memory leak test
            PyEnv pyEnv = new PyEnv();
            pyEnv.Init();
            pyEnv.Destroy();
        }
    }
}
