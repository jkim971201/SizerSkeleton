// OpenSTA, Static Timing Analyzer
// Copyright (c) 2024, Parallax Software, Inc.
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "ArcDcalcWaveforms.hh"


namespace sta {

Table1
ArcDcalcWaveforms::inputWaveform(const Pin *,
                                 const RiseFall *,
                                 const Corner *,
                                 const MinMax *)
{
  return Table1();
}

Table1
ArcDcalcWaveforms::drvrRampWaveform(const Pin *,
                                    const RiseFall *,
                                    const Pin *,
                                    const RiseFall *,
                                    const Pin *,
                                    const Corner *,
                                    const MinMax *)
{
  return Table1();
}

} // namespace
