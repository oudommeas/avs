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
package com.waz.media.manager.player;


import com.waz.media.manager.player.MediaSourceListener;


public interface MediaSource {
  public void play ( );
  public void stop ( );

  public MediaSourceListener getListener ( );
  public void setListener ( MediaSourceListener listener );

  public float getVolume ( );
  public void setVolume ( float volume );

  public boolean getShouldLoop ( );
  public void setShouldLoop ( boolean shouldLoop );
}
