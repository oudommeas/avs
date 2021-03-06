/*
* Wire
* Copyright (C) 2016 Wire Swiss GmbH
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*/
#include <re.h>
#include <avs.h>
#include <gtest/gtest.h>


TEST(vidcodec, good)
{
	struct list vidcodecl = LIST_INIT;
	static struct vidcodec vc_good = {
		.pt = "100",
		.name = "VP8",
	};

	vidcodec_register(&vidcodecl, &vc_good);

	ASSERT_EQ(1, list_count(&vidcodecl));

	vidcodec_unregister(&vc_good);
}


TEST(vidcodec, bad)
{
	struct list vidcodecl = LIST_INIT;
	static struct vidcodec vc_bad = {
		.pt = "96",    /* PT reserved for Audio */
		.name = "VP8",
	};

	vidcodec_register(&vidcodecl, &vc_bad);

	ASSERT_EQ(0, list_count(&vidcodecl));
}
